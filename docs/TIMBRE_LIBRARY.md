# Banque TIMBRE

La banque contient 68 timbres, dont 43 nouveaux, accessibles dans TIMBRE et MIDI SCORE par **Family → Subfamily → Timbre**. Parcourir une famille ou une sous-famille ne modifie pas le son : seul le choix d'un timbre applique ses réglages.

| Famille | Sous-familles |
| --- | --- |
| Bass | Electric, Acoustic, Synth |
| Keys | Acoustic pianos, Electric pianos, Organs, Plucked keys |
| Guitars & plucked | Acoustic guitars, Electric guitars, Harp & folk |
| Strings | Solo bowed, Ensembles, Plucked |
| Woodwinds | Flutes, Single reeds, Double reeds |
| Brass | Solo brass, Ensembles |
| Voices | Solo vowels, Choirs |
| Synthesizers | Basic waveforms, Leads, Pads |
| Percussion | Drums, Mallets, Bells, Cymbals, Gongs |

Les douze basses sont Finger, Pick, Slap, Muted, Fretless, Upright pizzicato, Double bass bowed, Sub, Analog, Square, FM et 808. Les autres ajouts comprennent notamment guitares nylon et acier, harpe, mandoline, pianos felt et bright, clavinet, clavecin, hautbois, basson, sax ténor, flûte à bec, trompette, trombone, cor, tuba, alto, cordes pizzicato, voyelles oo/ee, chœur, pads, leads, grosse caisse, vibraphone et kalimba.

Les réglages sont regroupés dans cinq onglets de hauteur fixe : **Tone**, **Envelope**, **Filter**, **Motion**, **Texture**. Le niveau reste accessible dans tous les onglets. Les commandes de lecture et d'export restent sous l'éditeur. Le caractère spectral est indépendant de l'identité du preset : modifier un timbre en Custom conserve sa recette instrumentale.

## Évolution sonore

- Empreintes harmoniques distinctes pour cordes, basses, anches, cuivres, pianos, orgue et FM ; trois résonances fixes pour les voyelles.
- Filtre passe-bas à 24 dB/octave avec enveloppe de brillance : une attaque riche peut évoluer vers une tenue plus ronde sans écourter l'enveloppe d'amplitude.
- Transition de hauteur à l'attaque pour pincement, fretless, grosse caisse et basse 808.
- Réduction des textures de bruit des instruments tonals ; référence de normalisation corrigée pour ne pas amplifier le souffle d'un spectre faible ; coloration du bruit sans amplification cumulative vers les extrêmes de la bande.
- Marge pour les résonances du corps, fondamentale accordée même avec étirement des harmoniques, et réglage HF damping effectif sur les tables de cloches et percussions.

Il s'agit de recettes de synthèse spectrale, pas de banques d'échantillons enregistrés. Les différences de rendu sont testées numériquement ; leur qualité musicale doit aussi être évaluée à l'écoute dans la chaîne utilisée.

## Bruit, attaques et largeur harmonique

Le niveau Noise/Burst désigne désormais le **niveau RMS total du bruit** relativement à un partiel de référence. La distribution entre les bandes de fréquences est normalisée : elle n'applique plus ce niveau indépendamment à des centaines de bandes. Les textures suivent le filtre et, pour les voyelles, les formants. Un bruit situé sous le seuil représentable de l'image reste silencieux.

Les grains interpolent leur puissance en douceur entre les cellules de 4 ms. Les enveloppes sont évaluées au temps de chaque colonne, avec une montée progressive, y compris pour Burst. Le découpage d'une note entre deux fenêtres de rendu conserve sa texture.

Les traits intègrent une distribution gaussienne de puissance sur la surface de chaque pixel. Leur puissance totale reste constante quand leur largeur, leur position entre les pixels ou la résolution varie. Les harmoniques supérieures sont plus étroites, et la largeur s'ouvre progressivement après l'attaque. La référence est un oscillateur du lecteur à 3456 pixels ; le niveau ne dépend plus du nombre de pixels couverts par le trait.

Les presets de voyelles et de chœur désactivent le bruit ajouté par défaut. Le marimba utilise les modes de la barre, sans impulsion de bruit blanc : attaque de 7 ms, arrivée progressive des modes supérieurs, modes aigus atténués et filtre plus doux.

Mesures de régression sur des signaux synthétiques, après le décodeur dB de production :

| Mesure | Avant correction | Après correction |
| --- | --- | --- |
| Variation de niveau, largeurs 0,10–0,80 mm et hauteurs 864/3456 | 8,33 dB | 0,13 dB |
| Variation lors du déplacement entre les centres des pixels | 8,95 dB | 0,10 dB |
| Bruit demandé à −6 dB, puissance totale relative à la référence | +28,29 dB | −6,01 dB |
| Plus grande discontinuité relative d'amplitude aux frontières de grains testées | 0,642 | 0,018 |

La mesure du bruit est aussi vérifiée après conversion réelle de l'image en trames par `ScorePlayerService::buildFramesFromImage` : −6,01 dB. Ces nombres ne sont pas des niveaux dBFS en sortie du plugin et ne remplacent pas l'écoute de la chaîne complète.

## Sessions et registre

Les identifiants historiques 0 à 24 sont conservés ; les nouveaux timbres occupent les identifiants 25 à 67. Les sessions stockent les paramètres, pas uniquement le nom du preset. Les nouveaux champs absents d'une ancienne session prennent des valeurs neutres. Les corrections du moteur commun s'appliquent toutefois aussi aux anciens timbres.

Pour utiliser la nouvelle recette d'un ancien instrument ou d'un Custom, resélectionner son timbre dans le navigateur. Ce choix conserve la note, le niveau et l'activation ; il ne transpose jamais les notes MIDI. Le registre conseillé figure dans l'infobulle du timbre : par exemple E2 pour les basses. La page TIMBRE initiale propose désormais piano, basse, guitare nylon, flûte, violon et marimba dans leurs registres respectifs.

## Vérification

Depuis la racine Sp3ctra_VST, après compilation macOS avec les Makefiles CMake :

```sh
python3 vst/tests/timbre/run.py vst/build /tmp/sp3ctra-timbres
```

Le test utilise le moteur de rendu et le navigateur réels. Il vérifie les 68 rendus distincts et reproductibles, le classement et l'accès à chaque timbre, les douze basses, l'accordage, la sauvegarde/restauration, l'évolution du filtre, la normalisation du bruit, l'amortissement des tables et l'absence de modification lors de la navigation. Il exporte une planche `timbre-bank.png` et une capture `timbre-browser.png`.

Les tests de bruit exportent aussi `marimba-attack.png`, `vocal-ah.png` et `white-noise.png`. Ils vérifient la conservation de puissance, le niveau total du bruit, la continuité des grains, les attaques, le filtrage vocal et le découpage des fenêtres.
