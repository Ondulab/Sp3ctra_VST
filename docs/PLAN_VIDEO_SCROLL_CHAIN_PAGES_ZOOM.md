# PLAN — VIDEO SCROLL : pages par chaîne (VIDEO MIX ⇄ rack), zoom étendu et centre de génération

**Date : 2026-08-28 — Chantier VIDEO SCROLL, volet navigation + géométrie de sortie**

> **État au 2026-08-28** — V1 à V3 implémentés (build Release vert, cible
> Standalone). V4 : la vérification visuelle (§5) reste à faire par
> l'utilisateur — la capture d'écran est impossible sur la machine de
> développement (écran DisplayLink) et l'app n'a volontairement pas été
> lancée par l'agent.

## 1. Contexte et objectif

Trois demandes liées, toutes sur le module de sortie VIDEO SCROLL :

**A. Navigation « une seule interface »**
- Un clic sur le bandeau **VIDEO MIX** (zone 4) doit montrer en zone 3 les
  réglages PLAY de **tous** les VIDEO SCROLL patchés.
- Un clic sur une ligne **CHAIN n** de la tranche VIDEO MIX doit montrer les
  réglages du VIDEO SCROLL de cette chaîne.
- Un clic sur le module VIDEO SCROLL d'une chaîne (rack) ouvre la **même**
  interface : l'onglet PLAY est remplacé par le **nom de la chaîne**, et les
  autres chaînes sont cliquables à côté pour voir leurs réglages.
- Conséquence : l'onglet **SETUP disparaît** pour ce module ; son contenu
  (couleur de fond/cadre) est rapatrié sur la page, **minimisé** (le
  `ColourSelector` plein cadre occupe 352 px pour trois nombres).

**B. Zoom étendu + centre de génération**
- Pouvoir réduire le zoom bien en dessous du minimum actuel (0.5×).
- Deux nouveaux paramètres **Center X / Center Y** : position du centre de la
  génération (la ligne de naissance et sa bande) dans la fenêtre de sortie.

**C. Le balayage ne s'arrête plus au bord du cadre de zoom**
- Aujourd'hui, à zoom < 1, l'image entière (historique compris) est un petit
  rectangle centré ; le balayage s'arrête à son bord. Voulu : un balayage
  vertical en forte réduction continue de balayer **toute la hauteur** de la
  fenêtre de sortie (idem horizontal sur la largeur).

## 2. État des lieux (inventaire)

### 2.1 UI zone 3 / zone 4
- `video/VideoScrollPage.h` — page PLAY par instance (`setSlot(slot)`,
  banque `videoScroll{slot}_*`) : Mode / Speed / Line Pos / Thickness / Zoom /
  Fade / Blur / Gamma / Compression / Invert / Color. `kPreferredH = 504`.
- `ui/setup/VideoScrollSetupPanel.{h,cpp}` — face SETUP : un
  `juce::ColourSelector` plein cadre lié à `bgR/bgG/bgB` (`kPreferredH = 352`).
- `PluginEditor.h:231` `FaceSwitchBar` — segments PLAY | SETUP en dur
  (`setFace`, `setPlayOnly`, `onFaceChanged`).
- `PluginEditor.cpp` — câblage : `onVideoBlockSelected` (L61 : `setSlot` page +
  setup), `blockHasSetup()` (L975 : VideoScroll a un SETUP), `applyZone3Visibility()`
  (L1037/1058), `layoutZone3()` (L1943/2001), persistance `selVideoSlot` /
  `selSetupFace` (L2084).
- `ui/VideoMixerColumn.cpp` — bandeau : « VIDEO MIX » peint en texte
  (`paint`, L~205), **non cliquable** ; boutons REC / ⏸ / ■ / détacher /
  plein écran / replier à droite.
- `ui/VideoMixerComponent.cpp` — tranche : une ligne par sortie, libellé
  « CHAIN n » (+ suffixe a/b si plusieurs sondes dans une chaîne) peint dans
  `paint()` sur `kLabelW = 58` px, **non cliquable** ; ordre = ordre du rack
  (`processor.activeVideoSlots()` → `{slot, chainIdx}`).
- `ui/ChainRackComponent` — `selectInstance(id, notify)` déclenche
  `onVideoBlockSelected(slot)` **avant** `onBlockSelected` ; `setSelectedBlock`
  garde l'instance courante si elle est du bon type, sinon la première.

### 2.2 Rendu (`video/VideoScrollRenderCore.{h,cpp}`)
- Canevas interne = dimensions budgétées de la vue (`setDisplaySize`, W/H
  échangés en 90°/270°) ; historique = 4 × H lignes RGB, défilé **en place**
  autour de la ligne de naissance (`birthY = posNorm × bufH_`).
- `buildWarp()` : canevas complet (H lignes) ; compression / fade / blur sont
  des fonctions de la distance à la ligne de naissance, **spans = distance aux
  bords du canevas**.
- `drawWarp()` : **le zoom est une mise à l'échelle uniforme au dessin**
  (`s = fit × zoom`, centrée fenêtre) → à zoom < 1 tout le rendu (historique
  inclus) devient un rectangle réduit ; le reste = couleur de cadre
  (`bgR/G/B` via la loi Invert/Color).
- Signature de re-rendu du mixeur (`VideoMixerComponent::renderFrame`) :
  `level, blend, enabled, zoom, mode` par couche.

### 2.3 Paramètres
- `PluginProcessor.cpp:1744` — banque `videoScroll{n}_*` : `zoom`
  `NormalisableRange(0.5, 4.0, 0.05)` défaut 1.0 ; `bgR/bgG/bgB` 0..1.
- `ui/ModuleParamManifest.h:313` — `kVideoScroll[]` (VALUES de chaîne,
  presets `.sp3chain`, mémoire de type).
- Les VALUES de chaîne stockent des **valeurs brutes** et ignorent les
  propriétés absentes (`projectChainValuesToBanks`) → élargir une plage ou
  ajouter un paramètre à défaut ne demande **aucune migration**.

## 3. Décisions de conception

### D1 — Onglets de chaîne à la place de PLAY | SETUP (module VIDEO SCROLL)
Pour le bloc VideoScroll, la barre de faces affiche
**`[ALL] [CHAIN 1] [CHAIN 2] …`** — une entrée par sortie patchée, libellés
**identiques** à la tranche VIDEO MIX (suffixe a/b quand une chaîne héberge
plusieurs sondes), dans l'ordre du rack. `FaceSwitchBar` gagne un mode
« segments personnalisés » (`setCustomSegments(labels, sel)`,
`onSegmentSelected(int)`) ; les autres blocs gardent PLAY | SETUP à
l'identique. `blockHasSetup(VideoScroll)` passe à **false** ;
`VideoScrollSetupPanel` est **supprimé** (fichiers + CMake).

### D2 — Vue ALL
Nouveau `video/VideoScrollAllPage.h` : empile une `VideoScrollPage` par
sortie, chacune coiffée d'un en-tête « CHAIN n » (couleur du module)
cliquable → bascule sur l'onglet de cette chaîne. Deux colonnes quand la
largeur de page le permet (`≥ 2 × kMaxContentW + 16`), sinon une.
`refresh(activeVideoSlots)` reconstruit seulement si l'ensemble change.

### D3 — Points d'entrée de la navigation
| Geste | Effet |
|---|---|
| clic sur « VIDEO MIX » (bandeau zone 4) | `showVideoAllView()` : onglet ALL, bloc VideoScroll sélectionné dans le rack (instance courante si déjà VideoScroll, sinon la première) |
| clic sur « CHAIN n » (tranche zone 4) | `chainRack->selectVideoSlot(slot)` → même chemin qu'un clic rack : le rack se surligne, zone 1 suit, onglet de la chaîne |
| clic sur le module VIDEO SCROLL (rack) | onglet de sa chaîne (chemin existant `onVideoBlockSelected`) |
| clic sur un onglet chaîne | `selectVideoSlot(slot)` ; clic sur ALL → `showVideoAllView()` |
| en-tête d'une section de la vue ALL | onglet de cette chaîne |
| MIDI-follow sur un param `videoScroll{N}_*` | onglet de la chaîne concernée (chemin existant) |

Persistance session : `selVideoAll` (bool) en plus de `selVideoSlot`.
Les libellés/onglets sont rafraîchis sur `onModelChanged` et
`onStateRestoredUi` (module ajouté/retiré/déplacé).

### D4 — Couleur de fond minimisée
Une ligne **Background** dans la section DISPLAY : un bouton-pastille peint
avec la couleur courante (espace SOURCE, comme avant) qui ouvre un
`ColourSelector` dans une `CallOutBox` ; écriture dans `bgR/bgG/bgB`, la
pastille suit les changements externes (listeners APVTS). Hors du cadre
graphique, conformément aux règles UX du projet. Info-bulle : la couleur suit
Invert / Color comme l'image.

### D5 — Plage de zoom
`zoom` : `NormalisableRange(0.05, 4.0, 0.01, skew)` avec skew tel que le
centre physique du slider soit **1.0×** (double-clic `Sp3ctraBarSlider`
min → centre → max retombe sur 1.0×). Bornes partagées via
`VideoScrollLimits::kZoomMin/kZoomMax` (`video/VideoScrollMode.h`). Aucune
migration (valeurs brutes).

### D6 — Nouveaux paramètres `centerX` / `centerY`
`videoScroll{n}_centerX`, `_centerY` : −1..+1 (pas 0.01), défaut 0, libellés
« Center X » / « Center Y », ajoutés au manifeste `kVideoScroll`
(presets/mémoire de chaîne) et MIDI-learn (clic droit). Exprimés dans
**l'espace de la fenêtre de sortie** (X → droite, Y → bas) quelle que soit
l'orientation : ±1 place le centre de la bande **sur le bord** de la fenêtre.
Conversion vers le repère canonique (avant rotation) par la rotation inverse
de l'angle (`centreOffset`, cf. § 6 ter — remplace la table à 4 modes).

### D7 — Modèle de rendu « hybride » (transverse rigide, axe de défilement = fenêtre)
Le canevas interne reste **la fenêtre entière** (W × H budgétés) — c'est ce
qui fait que le balayage couvre toute la fenêtre. Le zoom agit différemment
sur les deux axes :

- **Axe transverse (largeur de la ligne)** — au dessin, rigide : les colonnes
  du canevas sont mises à l'échelle `z` autour de leur centre puis décalées de
  `cxc × W/2`. Un changement de zoom / centre / couleur de cadre est donc
  **instantané** sur toute l'image (comme aujourd'hui), sans réallocation.
- **Axe de défilement (temps)** — 1 ligne de canevas = 1 pixel de fenêtre,
  jamais mis à l'échelle au dessin. Le zoom s'applique aux grandeurs
  **au tamponnage** pour conserver le rendu d'une réduction uniforme :
  vitesse `px/s × z`, épaisseur `1 + t × (z·H − 1)` (bornée à H). Le rayon de
  blur n'est pas modifié (l'axe transverse est déjà scalé au dessin).
- **Ligne de naissance** — position normalisée
  `b = clamp(0.5 + cyc/2 + (posNorm − 0.5) × z, 0, 1)` : le « cadre de zoom »
  (z·H de haut, centré par Center Y) fixe où vit la ligne via Line Pos, mais
  l'historique s'écoule au-delà, jusqu'au bord du canevas — spans de
  compression / fade / blur = distances aux **bords de la fenêtre**.
- `drawWarp` : `T = translate(−W/2, −H/2) · scale(z, 1) · translate(cxc·W/2, 0)
  · rotate(mode) · scale(fit) · translate(centre fenêtre)` ; le zoom n'entre
  plus dans `fit`.

Justification : coût CPU et mémoire **constants** quel que soit le zoom (un
modèle rigide sur les deux axes exigerait `H/z` lignes de warp — 20 × à
0.05×), aucune réallocation de l'historique sur un balayage de paramètre,
et le rendu **à l'intérieur** du cadre est celui de la réduction uniforme.

### D8 — Compatibilité / propriétés assumées
- `zoom = 1`, `center = 0` → rendu **identique** à aujourd'hui (même
  transformée, même ligne de naissance).
- `zoom < 1` → l'historique remplit désormais la fenêtre (c'est la demande C).
- `zoom > 1` → la bande est recadrée comme aujourd'hui ; l'axe temps n'est
  plus agrandi mais la vitesse l'est (même densité visuelle, plus net).
- Un changement de zoom **en cours de défilement** ne remet pas à l'échelle
  la densité temporelle des lignes déjà tamponnées (même nature que changer
  Speed). Documenté, assumé.
- La signature du mixeur ajoute `centerX/centerY` ; le cache de `buildWarp`
  remplace `linePos` par la position résolue `b` (couvre linePos, zoom,
  centerY, mode).

## 4. Jalons

### V1 — Rendu + paramètres
- `video/VideoScrollMode.h` : `VideoScrollLimits::kZoomMin/kZoomMax` +
  helper `videoScrollOutputLabels()` (libellés partagés tranche / onglets /
  vue ALL).
- `PluginProcessor.cpp` : plage de `zoom`, création de `centerX/centerY`.
- `ui/ModuleParamManifest.h` : suffixes `centerX`, `centerY`.
- `video/VideoScrollRenderCore` : `canonicalCentre()`, `birthLine01()`,
  vitesse/épaisseur × z, cache warp sur `b`, `drawWarp` D7.
- `ui/VideoMixerComponent.cpp` : signature + `centerX/centerY`.

### V2 — Page PLAY
- `video/VideoScrollPage.h` : lignes Center X / Center Y (après Zoom) ;
  ligne Background (pastille + CallOutBox) ; `kPreferredH` recalculé ;
  MIDI-learn des nouveaux sliders.
- Suppression de `VideoScrollSetupPanel` (fichiers, CMake, éditeur).

### V3 — Onglets de chaîne, vue ALL, navigation
- `PluginEditor.h` : `FaceSwitchBar` mode segments personnalisés.
- `video/VideoScrollAllPage.h` (nouveau).
- `ui/ChainRackComponent` : `selectVideoSlot(slot)`.
- `ui/VideoMixerColumn` : `onHeaderClicked` (titre cliquable, survol) ;
  `ui/VideoMixerComponent` : `onOutputClicked(slot)` (libellés cliquables,
  survol).
- `PluginEditor.cpp` : `videoAllView_`, `showVideoAllView()`,
  `refreshVideoTabs()`, visibilité/layout, persistance `selVideoAll`,
  câblage des callbacks, `blockHasSetup`.

### V4 — Vérification (skill `verify`, `build_vst.sh --run`)

## 5. Plan de vérification

- **V-1 Navigation bandeau** : clic « VIDEO MIX » → zone 3 = vue ALL avec une
  section par sortie ; onglet ALL actif ; libellés = ceux de la tranche.
- **V-2 Navigation tranche** : clic « CHAIN 2 » → onglet CHAIN 2, page liée à
  la banque de ce slot (bouger Speed → le waterfall de cette sortie change),
  rack surligné sur ce module, zone 1 sur sa sonde.
- **V-3 Navigation rack** : clic module VIDEO SCROLL → onglet de sa chaîne ;
  clic sur un autre onglet → réglages de l'autre chaîne ; retour ALL.
- **V-4 Plus de SETUP** : aucun segment SETUP pour VideoScroll ; les autres
  blocs gardent PLAY | SETUP ; session ancienne avec `selSetupFace = true`
  sur un VideoScroll → atterrit sur la page.
- **V-5 Background** : pastille ouvre le sélecteur ; changer la couleur →
  bordure de la sortie mise à jour en direct ; valeur persistée (sauver /
  recharger) ; Invert Luminance + blanc → cadre noir (loi conservée).
- **V-6 Zoom 0.05** : bande étroite centrée ; balayage vertical (Scroll
  down) couvrant toute la hauteur ; horizontal (Scroll left) couvrant toute
  la largeur.
- **V-7 Center X/Y** : la bande se déplace en direct ; ±1 = centre sur le
  bord ; en 90°/270°, X reste « droite » et Y « bas » ; ligne de naissance
  suit Center Y + Line Pos.
- **V-8 Identité à zoom 1 / centre 0** : comparaison visuelle avant/après
  sur une session existante.
- **V-9 Persistance / presets** : `centerX/centerY` et zoom < 0.5 survivent
  à sauver/recharger et à un round-trip `.sp3chain` ; une session ancienne
  charge à center 0.
- **V-10 MIDI** : learn sur Center X, Zoom ; MIDI-follow atterrit sur le bon
  onglet.
- **V-11 Perf** : 3 sorties à zoom 0.05 et 4.0 — fréquence de rendu stable
  (aucun coût dépendant du zoom par construction).

## 6. Risques et points de vigilance

- **Deux `VideoScrollPage` par slot** en vue ALL (la page simple + la
  section) : deux jeux d'attachments APVTS sur les mêmes params — supporté
  par JUCE, mais les pages invisibles doivent rester légères (pas de timer).
- **Sélection rack en vue ALL** : le rack surligne UNE instance ; zone 1
  affiche sa sonde. Assumé (la vue ALL est une vue d'édition, pas de
  monitoring).
- **`setSelectedBlock` sans notification** : `showVideoAllView()` doit
  resynchroniser `videoSlotIndex_` sur l'instance réellement surlignée.
- **Ordre des callbacks** : `onVideoBlockSelected` (met `videoAllView_ =
  false`) précède `onBlockSelected` → `selectBlock` → `refreshVideoTabs()`.
- **Rotation et signes** de `centerX/centerY` (D6) — vérifier les quatre
  modes (V-7).
- **Littéraux non-ASCII** dans l'UI → `fromUTF8` (standard projet).
- CI Windows : aucun code plateforme touché ; passer la CI avant de pousser
  sur master (la release bêta se rafraîchit au push).

## 6 bis. Complément du 2026-08-28 — loi du Fade

Demande : « pousser bien plus fort le fade et le mettre après gamma ». Dans
`buildWarp()`, la chaîne par pixel devient **moyenne → gamma → désaturation →
atténuation** (le gamma, appliqué après, relevait les sombres et annulait le
fade). Loi remplacée par une décroissance exponentielle en distance à la
source (persistance de type phosphore) :
`dim = exp(−10 · fade · a)`, `sat = 1 − dim` (`a` = 0 à la ligne, 1 au bord).
À fade = 1 : 37 % à un dixième du parcours, < 1 % à mi-parcours ; à fade = 0.1
le bord garde encore 37 %. Constante `kFadeRate` dans le renderer.

## 6 ter. Complément du 2026-08-28 — rotation continue (remplace « Mode »)

Demande : « remplacer le mode scroll par la rotation continue 360 ».

- **Paramètre** : `videoScroll{n}_rotation` (0..360°, pas 0.1, défaut 0,
  MIDI-learn, manifeste) remplace le choix `mode` (purgé). Sens : degrés
  horaires à l'écran ; 0 = lignes neuves en bas / scroll up, 90 = à gauche,
  180 = scroll down, 270 = à droite (= ancien `mode × 90`). Migrations :
  `setStateInformation` (PARAM `mode` → `rotation`) et
  `ChainModel::migrateModuleValues` (VALUES / mémoire de type / presets).
  La page remplace le combo « Mode » par une barre « Rotation ».
- **Canevas carré sur la diagonale** : pour balayer toute la fenêtre à
  n'importe quel angle **sans réallouer** l'historique pendant une rotation
  animée, `setDisplaySize(w, h)` alloue `D × D` avec `D = ⌈hypot(w, h)⌉`
  (plus d'échange W/H à 90°/270°). Le mixeur budgète désormais la
  **diagonale** (`kMaxRenderDim = 1600` → le canevas ne dépasse jamais
  1600², même pire cas qu'avant). Contrepartie : ~13 % (16:9) à ~30 % (vue
  carrée) de résolution en moins sur la vue à 0° ; enregistrement à 1440p /
  2160p plus coûteux (buffer ≈ 2.3 ×) — à surveiller.
- **Spans visibles** : `visibleSpans()` = boîte englobante de la vue tournée
  dans le repère du canevas (`sx = W|cos| + H|sin|`, `sy = W|sin| + H|cos|`).
  Le warp et le blur ne traitent que les lignes `[rowLo, rowHi)` de ce span
  (coût ≈ `D × sy`, proche de l'ancien `W × H` à 0°) ; les distances
  d'aging (compression / fade / blur) atteignent ses bords ; la ligne de
  naissance est **bornée** à ce span (`birthLine01`). Le cache du warp
  inclut la rotation et les dimensions de vue. Ce span ne sert qu'à la
  VISIBILITÉ, jamais à dimensionner.
- **Cadre de zoom — invariant par rotation (2026-08-29)** : `frameSpans()`
  = les dimensions de la vue elle-même (`W` transverse, `H` le long de
  l'axe de défilement), quel que soit l'angle — un viseur de caméra qui
  tourne sur la scène. C'est lui que Zoom multiplie : largeur de bande au
  dessin, cadre hébergeant la ligne (Line Pos), épaisseur maximale. Avant,
  le cadre suivait la boîte englobante de la vue tournée et « respirait »
  avec l'angle (+41 % à 45° sur une vue carrée) — l'utilisateur : « à la
  rotation le zoom change, il ne faut pas avoir ce comportement ». Conséquence
  assumée : sur une vue non carrée à 90°, la ligne à zoom 1 mesure `W` (elle
  ne s'ajuste plus à `H`) ; on compense au zoom.
- **Dessin** : la ligne tamponnée sur `D` colonnes (indépendante de l'angle)
  est ajustée à `W × zoom` au dessin (`bandScale`), décalée de Center X
  (`centreOffset` = offset fenêtre tourné de −θ, exact à tout angle), puis
  le canevas est tourné de θ et posé 1:1 sur la diagonale de la vue. À
  zoom 1 la bande a la largeur de la fenêtre quel que soit l'angle — rigide
  sur l'historique, pas de « trompette » en rotation animée.

## 6 quater. Complément du 2026-08-28 — page graphique : l'éditeur VIEWPORT

Demande : « rendre l'interface plus intuitive, visuelle, graphique — un
repère orthogonal où l'on visualise et déplace / grossit / tourne la
vignette ».

- **`video/VideoScrollViewportEditor.h`** (nouveau) — le pad VIEWPORT en tête
  de page : la fenêtre de sortie dessinée à son **vrai rapport** (taille de la
  vue du mixeur : fenêtre détachée si ouverte, sinon l'aperçu carré de la
  colonne), remplie par la **dernière image de cette sortie seule** (la
  vignette vivante : plein niveau, sans blend, son propre papier — pas le
  composite VIDEO MIX, dont les autres chaînes brouillaient le cadre édité ;
  ajusté le 2026-08-28), voilée hors de la bande de balayage ; par-dessus, le
  **cadre de zoom** (« vignette » : z × la vue elle-même W × H — invariant
  par rotation, voir `frameSpans` —, centré par Center X/Y, tourné par
  Rotation) avec la ligne de naissance
  (Line Pos). **Indicateurs des paramètres sans poignée (2026-08-29,
  `drawFlowAndAging`)**, tous dessinés DANS la fenêtre, sur l'axe : la
  **barre d'épaisseur** (Thickness : bande translucide centrée sur la ligne,
  `1 + t·(z·H − 1)` px comme le tampon) ; de chaque côté de la ligne encore
  visible, une **règle de temps** de 8 graduations à intervalles d'historique
  égaux — elles se resserrent vers le bord quand Compression monte (inverse
  de `d + c·d²/S`), s'éteignent avec Fade (`exp(−10·fade·âge)`) et
  s'élargissent avec Blur ; des **chevrons de flux** (1 à 3 selon |Speed|)
  qui s'éloignent de la ligne, ou la rejoignent quand Speed < 0, ternes à
  Speed = 0. Remplace la flèche unique : le rendu pousse l'historique des
  DEUX côtés de la ligne (`scrollStep`), et la flèche pointait hors de la
  fenêtre dès que la ligne était bornée sur un bord (« quand on déplace le
  centre on perd la flèche »). Le survol d'une poignée affiche son nom et sa
  valeur (`handleReadout`) ; une **légende** (`drawKey`) occupe la zone
  libre à gauche de la fenêtre quand elle a ≥ 210 px. Même géométrie que
  `drawWarp` / `birthLine01` (spans visibles,
  offset de centre, clamp de la ligne à la fenêtre) : ce qu'on saisit est ce
  que la sortie fait.
- **Gestes** : glisser le cadre = Center X/Y ; un anneau de coin = Zoom
  (uniforme autour du centre du cadre) ; le levier sur le bord avant, ou un
  glissé n'importe où hors du cadre (cadran) = Rotation ; la ligne de
  naissance = Line Pos ; pincement trackpad ou ⌘/Ctrl + molette = Zoom (la
  molette seule continue de faire défiler la page, règle des éditeurs) ;
  double-clic = remise à zéro de ce qui est sous le pointeur (cadre →
  centre 0/0, coin → 1.0×, levier/extérieur → 0°, ligne → défaut) ; clic
  droit = MIDI Learn du ou des paramètres sous le pointeur (sous-menus quand
  il y en a plusieurs — `MidiLearnPopup::addItems/handle`, factorisés).
- **Couleurs** : fenêtre, bande, axe, chevrons, règle, barre, légende = couleur du module
  (affichage) ; cadre, ligne, coins, levier, nœud central = lime
  (`Sp3ctraHandles`). Aucune boîte dans le cadre.
- **Page** (`video/VideoScrollPage.h`) réécrite dans le **squelette standard**
  `ModuleChrome` : cadre VIEWPORT (240 px) → rangée géométrie (Rotation ·
  Zoom · Center X · Center Y · Line Pos) → rangée rendu (Speed · Thickness ·
  Compression · Fade · Blur · Gamma) → caption `--- VIDEO SCROLL ---` →
  rangée loi d'affichage (Invert · Color · Background). Les barres et le pad
  sont liés aux mêmes paramètres (l'un pilote l'autre). `kPreferredH` =
  `pageHeight(kEditorsH, 1)` = 382 px (448 avant).
- **Plomberie** : `video/VideoScrollPreviewSource.h` (interface :
  `requestOutputPreview(slot)` + `outputFrame(slot)` — image de la sortie
  seule —, compteur de trames, taille de vue) implémentée par
  `VideoMixerComponent` (`currentView()` factorisé du presenter). Les rendus
  « solo » sont à la demande : le pad redemande son slot à chaque tick tant
  qu'il est visible, le presenter replie les demandes de moins de 300 ms en
  masque de slots (`Renderer::setSoloMask`), et le thread de rendu publie
  ces sorties seules dans un petit pool par couche (`Layer::soloPool`, jamais
  d'écriture dans une image en cours de peinture) ; sans surcoût de warp —
  l'image solo sert de source au blend, et en chemin « une seule sortie à
  plein niveau » elle partage l'image du composite ;
  `VideoMixerColumn::mixer()` ; l'éditeur câble la source après la création
  de la colonne, la détache dans son destructeur (la colonne meurt avant les
  pages) et appelle `previewTick()` depuis son timer 20 Hz — la page reste
  **sans timer** (vue ALL : une page par sortie), et ne repeint que si une
  nouvelle trame a été publiée ou si la vue a changé de taille.
  `VideoScrollRenderCore::applyDisplayColour` devient public pour que le
  papier « pas encore de trame » du pad suive la loi Invert / Color.
- **À vérifier visuellement** (machine sans capture) : orientation de la
  flèche aux quatre angles cardinaux, position de la ligne de naissance à
  zoom < 1 avec Center Y ≠ 0, aspect du pad fenêtre détachée 16:9 vs aperçu
  carré, lisibilité des 6 barres de la rangée rendu à 560 px.

## 7. Hors périmètre / suites possibles

- Vue ALL « compacte » (une seule ligne de sliders par sortie) si le nombre
  de sorties dépasse ce que deux colonnes rendent lisible.
- Automations de Center X/Y depuis l'hôte (déjà possible : ce sont des
  `AudioParameterFloat`) — pas de courbe de lissage prévue.
- Un mode « rigide » optionnel (zoom rescalant aussi l'axe temps) n'est pas
  retenu : coût proportionnel à 1/zoom.
