# Deuxième lot d’optimisations — 7 septembre 2026

Suite au gain constaté à l’écoute après le premier lot, cette passe cible le producteur LuxStral. Elle complète [le premier lot](CORRECTIONS_PERFORMANCES_2026-09-06.md) et [l’audit](AUDIT_PERFORMANCES_2026-09-06.md).

## Point de départ observé

La séquence du journal local du 6 septembre vers 19 h 40 utilise 96 kHz, 32 samples, 3456 notes et 6 workers : le budget d’un bloc est de **333,33 µs**. Le callback est autour de 3 µs en moyenne, avec un P99.9 autour de 10 µs et un pic initial de 494 µs. Le producteur consomme environ 217–226 µs en moyenne ; des maxima de 353–432 µs subsistent après un pic initial de 1537 µs. La starvation est de l’ordre de 0,02–0,07 %, sans underrun CoreAudio rapporté dans cette séquence.

Ces observations orientent vers les pics et le coût du producteur. Elles ne prouvent pas l’absence de craquements et ne mesurent pas la température du processeur.

## Changements appliqués

| Priorité | Fichiers / fonctions | Changement | Gain visé / risque |
|---|---|---|---|
| P1 | `synth_luxstral.c`, `synth_luxstral_threading.c`, `synth_work_dispatch.*` | Suppression des deux barrières collectives dans le rendu. Un signal de départ par auxiliaire, un compteur atomique de fin, un seul signal de fin émis par le dernier auxiliaire. Le producteur calcule la partition 0. | Moins de réveils et de contention. Pour 6 partitions, 5 threads auxiliaires au lieu de 6 ; pour 1 partition, calcul direct sans réveil. Le producteur dépend encore de la ponctualité des auxiliaires. |
| P0 | `Sp3ctraSharedCore.cpp`, `synth_prepare_runtime()` | Création du pool et réservation des buffers de mélange/stéréo/gris avant le démarrage du producteur. Capacités fixes : 4096 samples et 3456 pixels. Préparation réutilisée lors d’un changement de buffer. | Retire ces allocations et la création initiale des threads du premier rendu. Environ 107 Kio pour les sept buffers, indépendamment du bloc actif. Les autres allocations du moteur ne sont pas toutes supprimées. |
| P0 | `synth_shutdown_thread_pool_impl()` | Arrêt par réveil des auxiliaires puis `join`, sans délai artificiel de 50 ms. Nettoyage limité aux threads réellement créés et libération de toutes les partitions, y compris celle du producteur. Nettoyage sur échec partiel d’initialisation. | Retire 50 ms imposées à chaque arrêt/reconstruction. Arrêt valide uniquement lorsque le producteur est hors d’un lot de travail, contrat respecté par les appelants. |
| P2 | `synth_luxstral_cleanup()` | Libération et remise à zéro des deux buffers de gris. Leur capacité maximale couvre aussi le passage de 200 à 400 DPI. | Évite leur conservation après l’arrêt et une capacité insuffisante si la résolution augmente. Cette petite allocation conservée n’explique pas à elle seule une montée progressive importante de mémoire. |
| Mesure | `multithreading.c`, `rt_profiler.*` | Horloge monotone pour la durée du producteur ; nouveau relevé `RT_PRODUCER` : moyenne, maximum, P95/P99/P99.9, marge, dépassements, pertes de télémétrie. | Permet de distinguer le callback court d’un producteur en retard. File fixe, sans log ni allocation à la publication. |

Le nombre configuré conserve le même découpage des notes, les mêmes graines RNG et le même ordre de sommation. Les anciennes fonctions de barrière restent disponibles comme référence du benchmark ; le rendu normal ne les appelle plus.

## Mesure du producteur

`RT_PRODUCER` reprend le format de `RT_BLOCK`. La durée inclut le travail entre le réveil du producteur et la fin de son itération : alimentation des moteurs, préparation de l’image, calcul LuxStral et publication. Elle **exclut le retard de réveil avant le début de l’itération** et ne correspond pas à du CPU cumulé sur tous les threads.

Le budget est `taille du bloc de synthèse / fréquence`. `deadlines` compte les durées mesurées dépassant ce budget : ce n’est pas le compteur d’xruns de l’hôte. `minHeadroom` peut devenir négatif. Les percentiles sont quantifiés à la microseconde ; le maximum garde la durée réelle. `dropped` et `histogramOverflow` ont la même signification que pour `RT_BLOCK`.

La file du producteur est propre au producteur partagé et vit pendant le processus. Elle n’est pas réinitialisée par `prepareToPlay()`, qui peut survenir alors que ce producteur tourne encore. Le lecteur de diagnostics remet à zéro les fenêtres ; une fenêtre peut donc chevaucher une reconfiguration. Le reste du cycle de vie du profiler global et l’usage multi-instance restent à revoir.

## Validation et benchmark

- **Standalone, AU et VST3 compilés en Release**, ainsi que les deux tests d’intégration et le benchmark. Copie automatique des plugins et incrément de version désactivés dans le cache de validation. Les artefacts sont dans `build/Sp3ctraVST_artefacts/Release/` ; aucune installation effectuée par cette validation.
- **ASan/UBSan puis TSan** : tests ciblés de publication/completion avec données ordinaires partagées, 1 à 16 partitions, 3 cycles, 500 lots par cycle, démarrage partiel et arrêt répété. Les tests précédents de mailboxes, métriques et handoff passent également. Cela ne constitue pas un passage de tout le plugin sous TSan.
- **Pool réel lié au moteur** : 1/2/6/16 partitions, 3 cycles, 100 rendus par cycle. Vérification de la réutilisation des buffers pour 1/64/512/4096/32 samples, refus de 4097, arrêt et libération des buffers.
- **Stockage Sampler** : test d’intégration d’import exact, préservation d’overdub, remplacement et libération réussi.
- **Comparaison avant/après des noyaux** LuxStral, LuxWave, LuxSynth et LuxGrain : sorties et états identiques bit à bit dans les scénarios existants.
- **Comparaison des ordonnanceurs** : mêmes noyaux LuxStral réels, 3456 notes actives, mêmes données, même QoS, 3000 blocs par mesure, 1/2/6 partitions × 32/64/128/256 samples. Empreintes binaires des deux canaux de chaque partition à chaque bloc et des états finaux identiques pour les 12 configurations.

Le benchmark est isolé, sans périphérique audio, sans interface et sans cadence imposée. Les deux ordonnanceurs utilisent la même QoS, sans appliquer les politiques Mach spécifiques du pool de production. La mesure de durée porte sur le lancement/calcul/attente ; le CPU de processus inclut aussi la vérification des empreintes. Ce protocole ne mesure ni la charge globale du plugin ni les xruns de l’hôte.

Résultat initial à **6 partitions** :

| Bloc | Moyenne barrières → dispatch | P99 barrières → dispatch |
|---|---:|---:|
| 32 | 177,48 → 161,49 µs | 246 → 173 µs |
| 64 | 336,61 → 324,68 µs | 371 → 347 µs |
| 128 | 686,57 → 683,60 µs | 741 → 1074 µs |
| 256 | 1372,02 → 1385,26 µs | 1535 → 1895 µs |

Une seconde série de **5 répétitions à 6 partitions / 32 samples**, en alternant l’ordre des deux ordonnanceurs, donne :

| Mesure | Barrières | Dispatch |
|---|---:|---:|
| Médiane des moyennes | 177,76 µs | 163,37 µs |
| Plage des moyennes | 176,20–198,18 µs | 161,57–165,50 µs |
| Médiane des P99 | 231 µs | 209 µs |
| Médiane du CPU de processus / prise | 2,862 s | 2,721 s |

Cela représente environ **8,1 % de temps moyen en moins** et **4,9 % de CPU de processus en moins** sur cette seconde série. Le P99 est moins bon dans une des cinq paires ; les gros blocs de la première série présentent aussi des queues plus défavorables. Le gain sur les pics n’est donc pas garanti pour toutes les charges. Les données complètes sont conservées : [matrice initiale](performance/2026-09-07-worker-benchmark.txt), [cinq répétitions](performance/2026-09-07-worker-benchmark-repeat.txt).

Commandes de reproduction depuis la racine du dépôt :

```sh
cmake -S vst -B build -DCMAKE_BUILD_TYPE=Release \
  -DSP3CTRA_COPY_PLUGIN_AFTER_BUILD=OFF -DSP3CTRA_INCREMENT_VERSION=OFF \
  -DSP3CTRA_BUILD_PERFORMANCE_TESTS=ON
cmake --build build --target Sp3ctraVST_All Sp3ctraWorkerPoolTest \
  Sp3ctraWorkerBenchmark Sp3ctraPerformanceStorageTest -j6
sh vst/tests/performance/run.sh
SANITIZER=thread sh vst/tests/performance/run.sh
./build/Sp3ctraWorkerPoolTest
./build/Sp3ctraPerformanceStorageTest
./build/Sp3ctraWorkerBenchmark
./build/Sp3ctraWorkerBenchmark 5 6 32
```

Exécuter les benchmarks après la fin des compilations et des sanitizers. Pour la comparaison des noyaux, `python3 vst/tests/performance/compare_baseline.py /chemin/vers/source-avant` utilise une copie préalable du répertoire `vst/source`.

## Points encore ouverts

- Les notifications MIDI/LFO `setValueNotifyingHost()` restent dans le chemin audio. Une correction doit préserver l’application immédiate des modulations, les listeners APVTS, l’automation et la sauvegarde d’état. Écrire directement dans les atomics APVTS puis notifier plus tard ne suffit pas : JUCE peut alors ignorer la notification parce que la valeur paraît déjà appliquée.
- Le changement à chaud du nombre de workers recrée encore le pool depuis le producteur, à une frontière de bloc. Le délai fixe de 50 ms est supprimé, mais la création de threads et les logs de reconfiguration peuvent encore provoquer un pic.
- Les reconfigurations de fréquences, le mode de capture diagnostique des oscillateurs, certains verrous/publications et les configurations globales demandent encore du travail.
- Le double buffer LuxStral reste inchangé dans ce lot. Il ne garantit pas une réserve suffisante pour les très gros blocs hôte, un export hors ligne ou des retards prolongés. Le délai de réveil du producteur reste à corréler aux compteurs de starvation et à System Trace.
- Le stockage REC du Sampler, le précalcul d’enveloppes et les optimisations SIMD/vidéo restent des étapes distinctes.

## Prochain essai utile

1. Rejouer le même scénario à **96 kHz / 32 samples / 6 partitions**, avec le même routage et la même interface visible, pendant 20–30 minutes.
2. Comparer `RT_PRODUCER`, `RT_BLOCK`, starvation et underruns, en séparant le démarrage du régime établi. Contrôler `dropped` avant d’interpréter les percentiles.
3. Comparer ensuite 2, 4 et 6 partitions à contenu identique. Faire les changements entre prises, puisque la reconstruction du pool peut encore interrompre momentanément la production.
4. Si la starvation persiste avec un producteur largement sous le budget, mesurer le retard de réveil et le transport avec System Trace avant de dimensionner une FIFO. Si les pics suivent MIDI/LFO ou une reconfiguration, traiter ces chemins en priorité.


**Retour de test du même jour :** une dégradation à 512 samples a révélé un déséquilibre de priorités non couvert par le benchmark homogène ci-dessus. Voir [le diagnostic et le correctif de coordination](DIAGNOSTIC_AUDIO_GRANULAIRE_2026-09-07.md).
