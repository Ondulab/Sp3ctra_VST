# Cadence VIDEO MIX — 8 septembre 2026

Le retrait des warps masqués réduit le travail, mais le test simulé de
commutation ne mesure ni les délais du thread graphique ni la cadence réelle
du LFO dans une session. Il ne valide donc pas la disparition du problème
signalé à partir de 3 Hz.

## Mesure isolée du rendu

Test des calculs réels de `VideoScrollRenderCore` et du blit JUCE, en Release
sur cette machine, avec une source synthétique de 2048 pixels (16 lignes par
image), vitesse 0,6, atténuation 0,5 et une seule sortie visible. Les paramètres
sont fournis par un substitut minimal du processeur : aucun moteur audio, hôte
ni affichage de fenêtre ne tourne pendant ce test.

Sur quatre sorties, les moyennes tick + warp + blit sont de 1,09 ms sans flou,
1,52 ms à mi-flou et 1,95 ms au flou maximal (120 passes par configuration).
Ces mesures ne reproduisent pas le plafond de 3 Hz et ne constituent pas une
mesure de charge CPU en session. Détails dans
`performance/2026-09-08-video-render-baseline.txt`.

## Relever le problème en session

Fermer l'autre instance Standalone puis lancer depuis la racine du dépôt :

```sh
./scripts/diagnose_video_mix.sh
```

Le script lance l'application compilée dans `build-mac` (ou `build` à défaut)
avec `SP3CTRA_VIDEO_TIMING=1`. Reproduire à 3 Hz puis à 8 Hz, puis quitter.
Le chemin du journal temporaire est affiché au lancement. Les lignes
`[VIDEO TIMING]` donnent toutes les deux secondes :

- `render` : images publiées par le thread de rendu, temps moyen/maximal de
  ses passes, intervalle maximal entre publications ;
- `present` : nouvelles générations traitées par `paint`, coût de dessin et
  intervalle maximal entre ces dessins. Ce sont les soumissions de JUCE,
  pas les présentations physiques de l'écran ni les trames de l'encodeur.

Des images figées sans automation ne sont volontairement pas republiées :
une cadence basse dans ce cas est normale. Les rapports du présentateur
n'arrivent que lorsqu'il peint ; un blocage complet sera rapporté au retour
du dessin. Le compteur de générations est échantillonné à côté de l'image :
une publication concurrente peut décaler le comptage d'une image.

Si `render` tient environ 60 images/s mais `present` ralentit, examiner le
thread graphique. Si les deux ralentissent, confronter les intervalles aux
temps de calcul pour distinguer calcul et attente. Si les deux tiennent la
cadence, vérifier le paramètre modulé, la forme du LFO et le lien audio/vidéo.

Le diagnostic n'augmente pas la fréquence de traitement. Les accumulations
et écritures sont désactivées sans la variable d'environnement. Il n'écrit
aucun journal depuis le thread audio.

## Versions utilisées

Lors du diagnostic, le Standalone dans `build` contenait la première
optimisation, mais celui de `build-mac` datait du 4 septembre. Les VST3/AU
installés dataient des 29–30 août. Ce décalage peut expliquer un essai sur
une ancienne version ; il ne prouve pas que c'est la cause du signalement.

## Journal de session reçu et correction du lien audio

Le journal de 12:43:08–12:43:25 montre, après démarrage, environ 54 images/s
publiées pour 1,65–1,70 ms de calcul moyen. Le présentateur ne traite que
42–48 nouvelles images/s, avec des intervalles maximaux de 40–52 ms malgré
un dessin généralement inférieur à 0,1 ms. Le pic initial de 143,5 ms
survient pendant le chargement des prises : il ne représente pas le régime
établi.

En parallèle, `Config updated` revient 16–17 fois par seconde. La session
utilise un LFO carré à 8,365 Hz sur Focus X, avec le lien VIDEO MIX → AUDIO
armé. Chaque bascule du projecteur appelait `setChainVideoWeights`, qui
demandait un `applyConfigurationToCore` complet via le timer du processeur.
Le déplacement du mix réécrivait donc aussi les configurations de tous les
inserts et ajoutait un délai avant application des poids audio.

La première correction a supprimé ces reconstructions générales en ne
réécrivant que les gains. Le journal suivant confirme la disparition de la
rafale de configurations, mais montre encore environ 54–55 images/s au rendu
contre 30–45 côté JUCE, avec des trous ponctuels jusqu'à 359 ms.

## Présentation native et suivi sur l'horloge audio

Sur macOS, le rendu publie désormais directement l'image finie vers une couche
Core Animation hébergée dans une NSView, pour la prévisualisation comme pour
la fenêtre détachée. Il n'attend plus le timer JUCE puis `repaint` et `paint`.
Les changements utilisent des transactions explicites sans animation ; c'est
le mécanisme prévu pour un thread sans boucle Cocoa ([documentation Apple](https://developer.apple.com/documentation/quartzcore/catransaction)).
Le fournisseur CGImage conserve le buffer JUCE jusqu'à sa libération par
Core Animation : pas de copie intermédiaire des pixels ni de réécriture
d'une image encore utilisée. La couche possède sa propre durée de vie pour
supporter la fermeture d'une fenêtre pendant une soumission.

La fréquence des passes lourdes, leur résolution et le nombre de threads
restent identiques. Le chemin natif évite aussi le redimensionnement CPU intermédiaire de la
prévisualisation, puisque Core Animation dimensionne le composite. Il
remplace la présentation logicielle ;
il ne déplace pas encore les warps vers Metal. Pour comparer avec l'ancien
présentateur :

```sh
SP3CTRA_VIDEO_NATIVE=0 ./scripts/diagnose_video_mix.sh
```

Avec le chemin natif, le diagnostic donne `native-submit` à la place de
`present`. Il mesure les soumissions Core Animation, **pas les rafraîchissements
physiques de l'écran**. Une source figée avec un LFO carré à 8 Hz peut ne
publier qu'environ 16 images/s : seules les deux transitions par cycle
nécessitent alors une nouvelle image.

Le lien VIDEO MIX → AUDIO est maintenant évalué juste après le LFO et ses
mappings, dans chaque bloc audio, même sans éditeur ouvert. Avec les
32 échantillons à 44,1 kHz du journal, cette décision a un pas de 0,726 ms ;
ce n'est pas une mesure de latence audio de bout en bout. Un seul mot atomique
contient les huit poids de chaînes, indépendamment des gains/mutes/solos de
base. Aucune écriture de configuration ni notification de paramètre n'est
nécessaire. Les poids ont 256 niveaux (extrémités 0 et 1 exactes), plus fins
que l'ancien seuil de variation de 1/128.

Les quatre mélangeurs appliquent ces poids aux chaînes, pas aux numéros de
banques de conditionnement. Les caches spectraux sont invalidés lorsque le
poids d'une chaîne participante change, même sur une image source figée.
Une chaîne étrangère au moteur n'invalide pas son cache : elle ne provoque
pas de FFT supplémentaire.

## Validation de cette correction

- Test macOS utilisant la classe native de production : redimensionnement,
  format couleur, conservation des buffers, fermeture de la vue et
  soumission après sa destruction. Pendant deux secondes sans traitement
  d'événements sur le thread principal : 120 soumissions, aucun échec
  d'acquisition dans un pool de trois images. Ce test observe la couche et
  ses buffers ; il ne mesure pas l'écran.
- Test de la loi du projecteur : 1 600 alternances entre chaînes 2 et 4,
  désarmement, topologie vide et plusieurs sorties sur une même chaîne.
- Régression des quatre mélangeurs réels sous ASan/UBSan : 1 600 bascules
  sans republier la source, gains, mute, désarmement, banques différentes des
  indices de chaînes, invalidation des caches et conservation du cache pour
  une chaîne sans rapport. Suite de régressions audio existante également exécutée.

Commandes reproductibles après compilation :

```sh
sh vst/tests/performance/run.sh
python3 vst/tests/video/run_native.py build-mac
```

La régularité visible dans la session complète et la charge CPU totale doivent
encore être comparées en situation réelle. Le retrait du passage par le
thread graphique supprime une attente identifiée ; l'échantillonnage du
rendu (environ 18 ms dans ces journaux) et le rafraîchissement de l'écran
restent des limites de précision visuelle.

Standalone, VST3 et AU recompilés dans `build-mac`. Les exécutables des
VST3/AU installés dans la bibliothèque utilisateur ont été comparés par
SHA-256 aux produits de compilation : identiques.

## Blocage lors du changement de vue

La présentation native de la colonne conservait sa dernière image lorsqu'on
passait à la fenêtre détachée. Avec les autres références de présentation ou
de prévisualisation solo, elle pouvait immobiliser le pool de trois images.
Le routage efface maintenant le contenu de l'ancienne surface sur le thread
de rendu, avant de chercher le prochain buffer. Les prévisualisations solo
expirées sont également libérées avant l'acquisition. Une acquisition ratée
conserve la demande de publication, même si la source est ensuite figée.
La fréquence, la résolution et la taille du pool restent inchangées.

Le test natif vérifie le cas des trois buffers retenus puis leur libération
par le routage de production. Il place aussi la fenêtre JUCE en plein écran
avant les 120 soumissions natives pendant deux secondes de thread principal
bloqué. Ce test passe ; il ne reproduit pas toute la session utilisateur et
ne mesure pas la cadence physique de l'écran.
