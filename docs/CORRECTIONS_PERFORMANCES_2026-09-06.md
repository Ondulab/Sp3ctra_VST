# Corrections de performances — 6 septembre 2026

## Portée

Les corrections ci-dessous appliquent les mesures directement vérifiables de l'[audit](AUDIT_PERFORMANCES_2026-09-06.md). Elles ne constituent pas la réalisation de toutes les refontes proposées dans cet audit. Les travaux restant nécessaires sont explicités à la fin.

Travail effectué sur l'arbre local **1.4.604**, en conservant les modifications préexistantes. Une copie des sources avant intervention a servi aux comparaisons de rendu. Compilation Release arm64 sur le Mac accessible, avec JUCE et TTS existants. Les versions installées dans les dossiers de plugins ne sont pas remplacées par ces builds de validation ; les binaires résultants sont dans `build/Sp3ctraVST_artefacts/Release/`.

## Changements appliqués

| Priorité | Fichiers / traitement | Correction et effet |
|---|---|---|
| P0/P1 | `synthesis/luxstral/vst_callback_sync.c`, `threading/multithreading.c` | Le producteur attend sur un sémaphore. Le callback signale sans attendre ni prendre de mutex utilisateur. Les notifications sont coalescées et les jetons drainés, y compris dans le chemin rapide. Un timeout n'autorise **aucun rendu** ; il sert à vérifier l'arrêt du thread. |
| P1 | `luxsampler/LuxSampler.cpp`, `ScorePlayerService.cpp` | Remplacement des boucles `yield()` à 1 kHz par des attentes temporisées en microsecondes. Horloge monotone pour le cadencement ; rattrapage borné pour éviter les rafales de ticks après un retard. Les timestamps de données conservent leur domaine d'origine. |
| P1 | `synthesis/luxstral/synth_luxstral_threading.c` | Un seul bloc temporaire par worker, réutilisé immédiatement pour chaque note. Suppression du maximum de volume inutilisé et de sa réduction. À 3456 notes, mémoire temporaire des ondes : **54 Mio → 96 Kio avec 6 workers** (128 Kio avec 8). |
| P1 | même fichier | Les notes dont le volume courant **et** la cible sont exactement nuls évitent les lectures de tables et les passes enveloppe/mixage. La phase avance avec la même récurrence flottante ; les modes de phase et le panoramique conservent leur état. Aucun seuil n'élimine le fond sonore de la loi de décodage. |
| P1 | `luxsampler/LuxSampler.h/.cpp` | Imports image, imports de sessions, imports de slots et copies réservent le nombre réel de frames. Pour 2645 frames de 10 384 octets : **environ 27,5 Mo au lieu de 1,87 Go**. REC prépare sa capacité avant publication de la commande ; l'allocation et l'initialisation mémoire ne détiennent pas le mutex d'ingestion. L'overdub conserve la prise existante. |
| P0 | `synthesis/luxwave/synth_luxwave_engine.c/.h` | Trois buffers échangent leur propriété entre producteur et lecteur. Une quatrième table, réservée à l'audio, conserve l'ancienne onde pendant tout le crossfade. Une nouvelle publication attend la fin du fade avant d'être appliquée ; les publications intermédiaires sont remplacées par la plus récente. |
| P0 | `synthesis/luxsynth/synth_luxsynth_engine.c/.h`, `utils/spsc_snapshot.h` | Publication de spectres dans trois emplacements à propriété exclusive. Suppression de la copie concurrente du payload derrière un seqlock. Conservation de la rampe spectrale existante et des champs optionnels entre publications. |
| P0 | `synthesis/luxgrain/synth_luxgrain_engine.c/.h` | Même transfert de propriété pour les lignes pliées, les effacements d'historique et les métadonnées. Configuration réservée au thread audio ; seul le nombre de bandes nécessaire au feed est publié atomiquement. |
| P0 | `processing/chain_plan.c/.h` | Trois snapshots avec compteurs de lecteurs. Le producteur écrit uniquement un emplacement non lu. Trois essais maximum côté lecteur, puis restitution de son dernier plan cohérent. Suppression du fallback qui copiait des données pendant leur écriture. |
| P0 | `PluginProcessor.cpp/.h`, `CisVisualizerComponent.cpp` | Paramètres LuxSynth mis en cache et appliqués depuis le callback, après les mappings MIDI/LFO, avant le rendu. La configuration ne dépend plus de la FFT ni de la présence de l'éditeur. |
| P0 | `PluginProcessor.cpp/.h`, moteurs inline | Rendu LuxSynth/LuxWave/LuxGrain par portions de 4096 samples maximum. Les rampes de mixage restent définies sur le bloc hôte entier. Bornes défensives dans les moteurs ; taille du producteur LuxStral plafonnée à sa capacité. |
| P0 | consommateur LuxStral de `PluginProcessor.cpp` | Conservation d'un curseur de lecture pour les petits blocs variables. Un bloc n'est libéré qu'après lecture complète. Les anciens blocs ne sont plus rejoués en boucle. En sous-alimentation : fondu de 32 samples vers zéro puis reprise progressive, avec compteur de starvation. Cela masque les discontinuités mais ne remplace pas les samples manquants. |
| P0/P2 | callback et `midi/MidiMappingEngine.h` | Les consommateurs Note/CC ignorent les événements longs avant construction d'un `MidiMessage`. Le buffer MIDI original est conservé. Suppression des allocations SysEx inutiles dans ces parcours. |
| P0/P2 | LuxStral | Suppression des traces `SRC-GATE`, de leurs sommes diagnostiques, et du log de premier rendu des workers. Les initialisations/reconfigurations comportent encore d'autres logs : voir limites. |
| P2 | visualiseur, `processing/luxsynth_feed.c` | Fenêtre de Hann calculée uniquement quand la taille FFT change. Suppression de la FFT dans `paint()`. La FFT d'affichage ne tourne que pour un panneau FFT visible ; une vue masquée ne se rafraîchit plus. Le feed audio reste indépendant. |
| Mesure | `utils/rt_profiler.*`, `utils/rt_block_metrics.*` | Temps de callback sur horloge monotone, budget basé sur la taille réellement reçue, moyenne/max/P95/P99/P99.9/marge minimale/deadlines. File SPSC fixe ; statistiques et logs hors callback. Compteurs partagés du profiler rendus atomiques. Budget des lecteurs Sampler/Score corrigé à 1 ms dans le détail par famille. |

## Mesures et validation

- Compilation des formats **VST3, AU et Standalone** (`Sp3ctraVST_All`), plus test de stockage.
- Tests C avec **AddressSanitizer + UndefinedBehaviorSanitizer**, puis **ThreadSanitizer** : publications concurrentes ChainPlan, crossfades LuxWave, spectres LuxSynth, lignes/configuration LuxGrain, transport des mesures et handoff LuxStral. Vérification des bornes des trois moteurs inline sur demande de 8192 samples.
- Test de stockage avec ASan/UBSan : capacité d'import exacte, conservation du contenu lors d'une réserve d'overdub, remplacement, libération.
- Comparaison binaire avant/après du worker LuxStral : 576 oscillateurs, mono/stéréo, blocs 1/17/64/256/4096, morphing, plusieurs modes de phase, notes actives et strictement silencieuses. Sorties, énergie et états phase/enveloppe **identiques bit à bit** dans ce scénario.
- Même comparaison LuxWave : 3 fréquences d'échantillonnage × 5 tailles de bloc × 8 voix, avec publications laissant se terminer le crossfade. Rendu **identique bit à bit** ; le cas concurrent défectueux est couvert séparément par les sanitizers.
- Même comparaison LuxSynth/LuxGrain : 3 fréquences × 5 tailles, spectres variables, changement du nombre de bandes, effacement d'historique. Rendu **identique bit à bit** pour ce scénario séquentiel.
- Microbenchmark du worker avec la moitié des notes strictement silencieuses : environ **0,50–0,55 s → 0,31–0,33 s** de CPU dans les exécutions locales, soit environ **38–40 %** de réduction sur ce scénario. Ce chiffre **n'est pas** le gain CPU global du plugin ; la loi de décodage peut maintenir beaucoup de notes au-dessus de zéro. Une première exécution sans ce grand ensemble silencieux donnait environ 5 % pour tampon/réduction seuls, estimation trop courte pour une promesse de gain.
- Test de 1000 handoffs avec consommateur cadencé à environ 1 ms : environ **0,01 s CPU** pour le processus sans TSAN, plutôt qu'une occupation permanente pendant les attentes. Tests des timeouts et de 10 000 signaux redondants également passés.

Ces tests ne remplacent pas une écoute prolongée ni une capture Instruments dans le DAW. Ils ne prouvent pas l'absence de toutes les data races du projet : les sanitizers portent sur les sous-systèmes testés.

### Reproduire les vérifications

Depuis la racine du dépôt :

```sh
sh vst/tests/performance/run.sh
SANITIZER=thread sh vst/tests/performance/run.sh
cmake -S vst -B build -DSP3CTRA_COPY_PLUGIN_AFTER_BUILD=OFF -DSP3CTRA_INCREMENT_VERSION=OFF -DSP3CTRA_BUILD_PERFORMANCE_TESTS=ON
cmake --build build --target Sp3ctraVST_All Sp3ctraPerformanceStorageTest -j 6
./build/Sp3ctraPerformanceStorageTest
python3 vst/tests/performance/compare_baseline.py /chemin/vers/la/copie/avant/source
```

La comparaison exige une copie de l'arbre **avant cette intervention**, avec ses changements locaux, et pas uniquement le commit Git. Le script extrait le corps réel du worker pour le compiler dans un pilote déterministe ; il neutralise les logs et le chargement de timbre utilisateur. Les chemins avec timbre utilisateur chargé ne font donc pas partie de cette comparaison.

Les options CMake d'incrément de version et de copie des plugins restent activées par défaut pour les builds ordinaires ; elles sont désactivées dans le cache de ce build de validation. La version source reste **1.4.604**.

## Lecture des nouvelles mesures

La ligne `RT_BLOCK` est produite par le timer de diagnostics, environ toutes les dix secondes pour la taille habituelle de bloc :

```text
n=... avg=...us max=...us P95=...us P99=...us P99.9=...us minHeadroom=...us deadlines=... dropped=... histogramOverflow=...
```

- `minHeadroom = B/Fs − durée` est calculé bloc par bloc ; une valeur négative signale un dépassement.
- `deadlines` compte les durées supérieures au budget, indépendamment des xruns signalés par l'hôte.
- Percentiles de la fenêtre, quantifiés à la microseconde. Le dernier bin correspond à 65 535 µs et plus ; `histogramOverflow` indique les durées au-delà de la plage, tandis que `max` garde la durée réelle.
- `dropped` signale une saturation des 8192 entrées de télémétrie. Le callback n'attend jamais le lecteur. Une fenêtre avec pertes ne permet pas de conclure que les pics manquants étaient faibles.
- La starvation LuxStral est comptée séparément : un callback respectant sa deadline peut encore manquer de données produites.

## Limites et suite du plan

Les points suivants **ne sont pas résolus par ce lot** et doivent rester dans le suivi P0/P1 :

1. **Notifications APVTS depuis MIDI/LFO** : `setValueNotifyingHost()` subsiste. Il faut séparer les valeurs utilisées par le DSP des notifications UI/hôte, en conservant l'instant d'application et l'arbitrage avec l'automation. Un simple report de tout le changement au timer introduirait une latence de modulation.
2. **Configuration globale et cycle de vie du producteur** : `g_sp3ctra_config`, les coefficients partagés, les initialisations paresseuses FFT/pool, changements de workers, mutex de données et barrières demandent encore une refonte. Le nouveau protocole des trois moteurs ne sécurise pas tous les autres états globaux.
3. **Transport LuxStral** : le curseur corrige la perte des restes de petits blocs et le fondu évite de boucler une vieille onde. Il reste un double buffer, pas une FIFO audio dimensionnée pour absorber des variations prolongées ou un export hors ligne plus rapide que le temps réel. Pour un gros bloc hôte, le reste peut manquer tant que le producteur n'a pas rendu ; il est alors atténué vers zéro, sans accès hors limites.
4. **Enregistrement Sampler** : les imports sont compacts, mais REC prépare encore la capacité maximale de 180 000 frames. Cette opération peut peser lourd sur la mémoire et l'interface ; une réserve par pages préparées en arrière-plan est l'étape suivante. La copie d'une prise pour agrandissement/overdub reste sous mutex.
5. **MIDI TAP / préécoute / autres publications** : capacité de sortie MIDI, notification des autres cibles, verrou de préécoute, tables de timbre utilisateur et données vidéo ne sont pas entièrement traités ici. La contrainte d'une seule instance reste applicable.
6. **Algorithmes et vidéo** : fusion des passes/SIMD, précalcul des courbes d'enveloppes, choix du nombre de workers, Audio Workgroups et rendu GPU restent à sélectionner et mesurer. Activer arbitrairement NEON, modifier les priorités ou changer le synthétiseur sans comparaison sonore n'a pas été retenu dans ce lot.

### Validation en session réelle, puis poursuite

1. Exécuter le nouveau binaire pendant 20–30 minutes dans le scénario qui craquait : même fréquence, buffer, routage, nombre de voix, lecteurs et vidéo. Comparer éditeur ouvert/fermé et vidéo active/inactive.
2. Relever `RT_BLOCK`, starvation et durées par famille ; capturer Time Profiler, System Trace et Allocations. Vérifier particulièrement jitter des lecteurs, réveil du producteur, pression mémoire lors de REC et modulation MIDI/LFO.
3. Traiter d'abord les notifications et la configuration si elles créent encore des pics ; remplacer le double buffer par une FIFO si la starvation persiste malgré les gains CPU.
4. Développer le stockage REC par pages et les optimisations de noyau selon les mesures. Utiliser les comparaisons binaires existantes comme garde-fou, complétées par des tests de modulation, de timbre et une écoute.


Suite : [deuxième lot du 7 septembre 2026](CORRECTIONS_PERFORMANCES_2026-09-07.md), concernant le pool LuxStral, la préparation des buffers et les percentiles du producteur.
