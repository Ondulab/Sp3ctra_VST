# FPS de l'affichage vidéo

`ADVANCED → Video display FPS` propose 10, 15, 20, 24, 30, 45 et 60 FPS.
La valeur initiale reste 60 ; le choix est appliqué immédiatement et mémorisé
sur cette machine (`video.displayFps`), indépendamment des sessions.

Le réglage plafonne les calculs de warp, rotation et composition du VIDEO MIX,
puis les soumissions de la prévisualisation intégrée ou de la fenêtre plein
écran. Il ne change ni les producteurs de lignes, ni les traversées des
chaînes, ni les alimentations des synthèses, ni la mise à jour audio de LINK.
Même l'historique visuel continue à absorber ses lignes au rythme existant,
environ 60 ticks/s : on calcule moins souvent son image finale, sans retarder
sa collecte. Le défilement garde son échelle de temps.

Le plafond est une cadence cible, pas une garantie de performance. Le compteur
VIDEO du pied de fenêtre indique les nouvelles images calculées ; une source
figée peut publier moins d'images que le plafond choisi. Les modulations audio
peuvent continuer rapidement pendant que l'image se rafraîchit lentement.

Pendant un enregistrement, le rendu destiné au fichier conserve sa cadence
antérieure, pour ne pas dégrader la capture. La présentation à l'écran reste
plafonnée, mais l'économie de calcul est alors moindre. Un dernier composite
non encore présenté reste en attente jusqu'au prochain créneau, même si la
source se fige entre-temps.

## Accélération ajoutée

Sur ARM64, le rééchantillonnage bilinéaire de rotation/zoom traite les quatre
canaux de chaque pixel avec les instructions NEON. Les trois interpolations
entières arrondies restent identiques à la référence. Le repli scalaire reste
utilisé sur les autres architectures. Il s'agit de calcul vectoriel CPU ; la
présentation native Core Animation reste en place. Les warps et le mélange
complet ne sont pas portés vers des shaders Metal par ce changement.

Référence des instructions : [Arm NEON Intrinsics](https://arm-software.github.io/acle/neon_intrinsics/).

## Validation et limites des mesures

- Tests sous ASan/UBSan : plafonds de 10 à 60 FPS, changement à chaud,
  absence de rafales de rattrapage après retard, maintien des ticks
  d'historique et de la cadence d'enregistrement ; un million de pixels
  comparés entre interpolation scalaire et SIMD, avec poids limites et
  intermédiaires.
- Comparaison des images complètes avant/après sur six cas (largeurs 640 et
  1280, trois angles) : pixels identiques. Sur cette machine, la passe de blit
  utilise environ 50–59 % de temps CPU en moins. Cela ne mesure pas le gain
  CPU total de l'application. Valeurs : `performance/2026-09-08-video-simd.txt`.
- Rejeu de `VideoScrollRenderCore` réel avec paramètres fournis par un
  substitut de processeur : 240 ticks d'historique et 3 920 lignes fournies
  dans chacun des quatre scénarios. Les passes d'image passent de 240 à 120,
  60 et 40 ; les images finales sont identiques. Temps CPU processus :
  1,348 / 0,692 / 0,356 / 0,246 seconde. Cette scène synthétique ne lance
  ni hôte, ni backend audio ; ce n'est pas une mesure de charge en session.
  Valeurs : `performance/2026-09-08-video-display-fps.txt`.

Commandes reproductibles après compilation :

```sh
sh vst/tests/performance/run.sh
python3 vst/tests/video/run_display_fps.py build-mac
```

Pour une comparaison en session, conserver les mêmes sources, le même zoom,
la même fenêtre et le même état d'enregistrement. Les débits C1…C8, les
alimentations des synthèses et les changements LINK/s permettent de vérifier
que le réglage FPS agit sur l'affichage, sans ralentir le flux audio.
