# Dégradation soudaine du son — diagnostic et correction

## Ce que le test établit

Le journal fourni montre une rupture vers **19:40:54**, après le passage à **96 kHz / 512 samples** à 19:38:48. Le budget est de **5,333 ms** par bloc.

| Fenêtre terminant à | Moyenne producteur | Maximum | Blocs produits | Dépassements mesurés |
|---|---:|---:|---:|---:|
| 19:40:48 | 2,867 ms | 4,086 ms | 1876 | 0 |
| 19:40:58 | 3,348 ms | 5,851 ms | 1589 | 284 |
| 19:41:08 | 5,405 ms | 5,946 ms | 963 | 917 |
| 19:41:38 | 5,399 ms | 6,129 ms | 968 | 907 |

À 19:41:08, le callback reste à **7,18 µs en moyenne**, avec un maximum de **36 µs** et aucune deadline dépassée. Il reçoit 1879 appels, tandis que seulement 963 nouveaux blocs sont produits. Le compteur cumulé de starvation passe de 284 à 1201 : **917 callbacks privés de nouvelles données dans cette fenêtre, soit environ 49 %**.

Le taux `starved` affiché dans l’ancien résumé est **cumulé depuis prepareToPlay**, pas propre aux dix dernières secondes. Les 4,57 % affichés à 19:41:08 sous-estiment donc visuellement la gravité immédiate. Il faut calculer les différences de compteurs pour cette analyse.

Avec le transport actuel, le callback atténue vers zéro lorsqu’il manque de données. Une alternance approximative d’un bloc sonore et d’un bloc manquant à 512 samples produit un hachage autour de 94 Hz. Cela explique le caractère granulaire décrit, même si CoreAudio ne rapporte aucun underrun : son callback reçoit bien un buffer à temps, mais celui-ci contient déjà le défaut.

La capture de charge globale, qui montre encore des ressources libres, ne garantit pas la disponibilité ponctuelle de chaque thread nécessaire au rendu. Elle ne donne pas non plus le nombre de threads de Sp3ctra seul.

[Mesures extraites du journal](performance/2026-09-07-granular-metrics.txt).

## Déséquilibre d’ordonnancement observé

L’application étant encore ouverte, un relevé `sample` de trois secondes a été réalisé sur le processus **96834**, à **19:44:22**. Une lecture des informations de threads avec `proc_pidinfo` a ensuite montré :

- producteur `Sp3ctraAudioProcessing` : priorité **46**, politique timesharing ;
- cinq auxiliaires de synthèse actifs : priorité **97**, politique temps réel ;
- callback CoreAudio : priorité **97**.

Dans le relevé du producteur, **451 des 1523 observations** le placent dans `semaphore_signal_trap`, appelé par `synth_work_dispatch_begin`. Le calcul de sa partition représente 578 observations, et l’attente de consommation 490. Ces observations décrivent des états échantillonnés, pas du CPU cumulé. Elles montrent toutefois un temps important passé au lancement des auxiliaires, avant le calcul de la partition du producteur.

Le deuxième lot avait déplacé une partition de calcul vers le producteur, tout en conservant cette différence de classe d’ordonnancement. Un réveil peut alors donner immédiatement la main aux auxiliaires prioritaires et retarder le producteur. Son calcul risque de s’ajouter au leur au lieu de les recouvrir. Le benchmark précédent utilisait une QoS homogène et ne couvrait pas cette combinaison de politiques Mach : c’était une limite importante de sa validation.

Autre défaut établi dans le code : les contraintes Mach des auxiliaires étaient calculées lors de leur création à **128 samples**, puis conservées après le passage à **512**. Leur période et leur réserve de calcul ne correspondaient donc plus au travail demandé.

Ces éléments étayent fortement un problème de coordination du producteur avec ses auxiliaires. Ils ne prouvent pas quel événement système précis a provoqué la transition à 19:40:54. Une variation de placement sur les cœurs, de concurrence ou de fréquence peut déclencher le franchissement du seuil. Aucune mesure thermique ne permet ici de conclure à un throttling, et aucune fuite mémoire progressive n’est démontrée par ces données.

[Extrait du relevé vivant et priorités](performance/2026-09-07-granular-live-excerpt.txt).

## Correction appliquée

| Fichiers / fonctions | Modification |
|---|---|
| `synth_luxstral_threading_rt.c` — `synth_update_realtime_team_policy()` | Applique une classe temps réel cohérente au producteur et aux auxiliaires, à une frontière de bloc, avant leur réveil. Même instantané de fréquence/taille pour toute l’équipe. |
| `luxstral_engine.h`, `synth_luxstral_threading.c` | Invalidation de la configuration d’ordonnancement lors d’une reconstruction du pool. Cache lié aussi au thread producteur : un nouveau thread reçoit sa politique même si le format est inchangé. |
| `synth_luxstral.c` | Actualisation des politiques lors du premier rendu et après un changement de fréquence ou de taille de bloc. Aucun appel de configuration Mach sur les blocs ordinaires sans changement. |
| `synth_luxstral_threading_rt.c` | Réserve de calcul portée de 50 à 75 % de la période courante, les rendus denses observés excédant déjà 50 %. La réussite requiert désormais celle de la politique temps réel, pas seulement du réglage de précédence. En cas de réussite partielle, tentative de retour de toute l’équipe en timesharing avec précédence normale. |
| `synth_luxstral.c`, `rt_profiler.*` | Mesures séparées `RT_LS_LAUNCH`, `RT_LS_OWN`, `RT_LS_JOIN`, avec le même format de percentiles que `RT_PRODUCER`. Publication dans des files fixes ; calcul des statistiques et logs sur le lecteur de diagnostics. |

`preemptible` est positionné à `TRUE` pour les noyaux qui respectent ce champ. La correction ne repose pas sur ce bit : [l’en-tête XNU actuel indique qu’il est ignoré](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/mach/thread_policy.h). Les politiques Mach sont des contraintes déclarées au système, pas une garantie d’absence de retard ; Apple décrit notamment la possibilité d’une rétrogradation en cas de comportement incompatible avec les besoins annoncés dans son [guide de l’ordonnanceur](https://developer.apple.com/library/archive/documentation/Darwin/Conceptual/KernelProgramming/scheduler/scheduler.html). Aucune rétrogradation des auxiliaires n’a été constatée dans notre lecture, où ils étaient toujours à 97.

Le DSP et l’ordre de sommation ne sont pas modifiés. Le transport double buffer reste inchangé ; il peut encore manquer de données lorsqu’un retard dépasse la réserve disponible. Cette correction ne garantit donc pas zéro craquement sous toute charge.

## Validation

Le nouveau test `Sp3ctraMacSchedulingTest` emploie le **pool et le noyau réels**, 3456 notes actives, 6 partitions, 512 samples à 96 kHz, 400 rendus par phase avec attente de cadence sans boucle active. Il compare :

1. Producteur en QoS et auxiliaires temps réel conservant une période de 128 samples et une réserve de 50 %, comme avant correction.
2. Équipe avec politiques temps réel cohérentes et actualisées.

Il lit les politiques Mach pour vérifier la cohérence après changement de fréquence, de buffer et remplacement du producteur à format inchangé. Les empreintes des deux canaux et des états finaux sont identiques.

| Exécution | Ancien : moyenne / P99 / dépassements | Corrigé : moyenne / P99 / dépassements |
|---|---:|---:|
| Première | 3,584 / 5,194 ms / 1 sur 400 | 2,855 / 3,414 ms / 0 sur 400 |
| Finale | 3,683 / 7,390 ms / 7 sur 400 | 2,887 / 3,363 ms / 1 sur 400 |

Sur la seconde exécution, le lancement passe de **61,47 à 6,57 µs en moyenne**. Le rendu moyen baisse d’environ **22 % dans ce test**. L’ancienne application était encore ouverte pendant ces mesures ; elles incluent donc une charge concurrente réelle et ne constituent pas un benchmark isolé reproductible au pourcentage près. Le test court n’a pas reproduit à l’identique le basculement persistant après plusieurs minutes, et un dépassement subsiste dans la phase corrigée finale.

- Comparaison binaire avant/après des noyaux LuxStral, LuxWave, LuxSynth et LuxGrain : réussie dans les scénarios existants.
- Test du pool réel 1/2/6/16 partitions, préparation de buffers et cycles de redémarrage : réussi.
- Compilation Release Standalone, AU et VST3 : réussie, dans `build/Sp3ctraVST_artefacts/Release/`. Aucune installation ni modification du processus déjà ouvert.

[Première exécution](performance/2026-09-07-macos-scheduling-first.txt) · [Exécution finale](performance/2026-09-07-macos-scheduling-final.txt).

Reproduction, après activation de `SP3CTRA_BUILD_PERFORMANCE_TESTS` dans CMake :

```sh
cmake --build build --target Sp3ctraMacSchedulingTest Sp3ctraWorkerPoolTest -j6
./build/Sp3ctraMacSchedulingTest
./build/Sp3ctraWorkerPoolTest
```

## Prochain essai

Fermer l’ancienne application, puis lancer **`build/Sp3ctraVST_artefacts/Release/Standalone/Sp3ctra.app`**. L’instance observée venait de **`vst/build/...`**, qui est un autre répertoire de compilation. Garder la même session et les réglages 96 kHz / 512 samples pendant au moins 20 minutes.

La ligne `SYNTH_SCHED` doit annoncer `producer + 5 auxiliaries: matched RT policy`, `96000 Hz / 512 samples`, `period 5.333 ms`. Examiner ensuite `RT_PRODUCER` et les nouvelles mesures :

- `RT_LS_LAUNCH` : temps de lancement des auxiliaires, y compris les suspensions éventuelles pendant les appels de réveil ;
- `RT_LS_OWN` : temps écoulé pendant le calcul de la partition du producteur, y compris ses éventuelles préemptions ;
- `RT_LS_JOIN` : attente résiduelle des auxiliaires après ce calcul.

Les trois histogrammes supplémentaires occupent environ 1,9 Mio fixes. Ils ne mesurent pas le CPU pur des workers. Les dépassements de chacune des étapes ne doivent pas être additionnés pour obtenir ceux du bloc complet : `RT_PRODUCER` demeure la référence pour ce total.

Si la starvation persiste alors que le producteur et son lancement gardent une marge confortable, la prochaine étape est le transport FIFO et la mesure du retard de réveil avant le début de l’itération. L’intégration des threads asynchrones à un workgroup adapté reste aussi à envisager selon la [procédure Apple](https://developer.apple.com/documentation/audiotoolbox/adding-asynchronous-real-time-threads-to-audio-workgroups).
