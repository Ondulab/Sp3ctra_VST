# Monitoring des débits de traitement

La barre au pied de la fenêtre montre des débits mesurés sur une seule ligne
de 28 pixels. Les chaînes absentes sont masquées. Si la fenêtre est trop
étroite, un défilement horizontal conserve toutes les valeurs lisibles.
Les couleurs reprennent celles des sources, du bandeau AUDIO MIX, des
modulations, des chaînes (`ChainIdentity`) et des sorties (`moduleColour`). Les producteurs
incrémentent des compteurs cumulatifs ; l'éditeur calcule les différences
sur le temps réellement écoulé, au plus deux fois par seconde. Les timers
et la fréquence du rendu vidéo ne sont pas accélérés par cette fonction.

| Champ | Point de mesure |
|---|---|
| INPUT, lines/s | Lignes complètes assemblées par le récepteur du capteur. Les sources internes ne sont pas incluses. |
| C1…C8, l/s | Lignes dont le traitement atteint la fin de cette chaîne, source, lecteur et queues d'effets compris. |
| Survol C1…C8 | Intervalle moyen entre fins de traitement et débit des lignes conditionnées publiées vers chacune des quatre sorties audio. |
| STRAL / SYNTH / WAVE / GRAIN, feeds/s | Publications de mix non vide vers le moteur correspondant. Stral et Wave peuvent republier une entrée inchangée ; Synth et Grain ignorent les générations inchangées. |
| AUDIO, blocks/s | Blocs du processeur passant par le traitement des modulations audio. La durée affichée est taille du bloc / fréquence d'échantillonnage. |
| LINK, changes/s | Modifications effectives des poids VIDEO → AUDIO. Un LFO carré à 8 Hz donne normalement 16 transitions/s. Une modulation continue n'a pas cette relation. |
| VIDEO, fps | Nouvelles images composites publiées par le moteur de rendu, pas les présentations physiques de l'écran. |
| HOLDS, /s | Lectures de staging différées à cause d'une écriture concurrente ; l'ancien mix est conservé. Ce ne sont pas des underruns audio. |

Ces valeurs ne mesurent pas la latence de bout en bout, ni le pire intervalle
entre deux lignes. Une moyenne de 1 000 lignes/s peut cacher des arrivées en
rafales. L'intervalle moyen affiché au survol ne doit pas être interprété
comme le délai capteur → haut-parleur. Les fréquences de notes ne sont pas
non plus les débits d'alimentation des synthèses.

Une chaîne peut changer de propriétaire au niveau d'un lecteur : le débit
C1…C8 compte seulement le propriétaire de la sortie finale, sans additionner
les segments amont. Une sortie audio placée avant ce lecteur peut avoir un
autre débit ; le survol expose justement ce cas. Les lignes répétées restent
des lignes traitées. Une chaîne sans signal qui ne traverse pas ses inserts
n'incrémente pas ce compteur.

Les compteurs du capteur, des chaînes, des sorties et des moteurs suivent le
cœur partagé entre instances. AUDIO et LINK suivent le processeur affiché ;
VIDEO suit son éditeur. Chaque éditeur conserve sa propre base de mesure :
ouvrir une fenêtre ne remet pas les compteurs des autres à zéro.

## Coût et validation

Les nouveaux points de mesure exécutent un incrément atomique 64 bits relaxé,
sans allocation, verrou, lecture d'horloge ni journalisation. Les compteurs
sont isolés sur des lignes de cache ; le calcul des débits et des textes est
fait par l'interface. Aucun calcul d'image supplémentaire n'est déclenché.
Ce coût est faible mais pas nul ; ce n'est pas une garantie de CPU total
strictement identique dans toute session.

Tests reproductibles :

```sh
sh vst/tests/performance/run.sh
python3 vst/tests/ui/run_metrics.py build-mac /tmp/sp3ctra-metrics-footer.png
```

La suite vérifie les incréments concurrents, les lecteurs indépendants,
l'arrêt, le redémarrage des compteurs et le retard de l'interface. Le test
staging vérifie que 1 600 remélanges d'une source figée ne sont pas comptés
comme 1 600 nouvelles lignes de la chaîne. Le test du composant réel vérifie
les valeurs affichées, le détail des sorties, le retour à zéro et l'absence
de texte coupé à la largeur minimale de 1 024 pixels grâce au défilement,
ainsi que l'alignement sur une ligne et les couleurs partagées avec le rack. Son image exportée
utilise des données de test ; ce n'est pas une mesure de session.
