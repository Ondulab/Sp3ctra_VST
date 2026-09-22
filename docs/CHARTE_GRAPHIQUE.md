# Sp3ctra VST — Charte graphique & inventaire des éléments d'interface

> Document de référence pour le design system de l'interface Sp3ctra VST.
> Source unique de vérité des tokens : [`vst/source/UITheme.h`](../vst/source/UITheme.h).
> LookAndFeel custom : [`vst/source/Sp3ctraLookAndFeel.h`](../vst/source/Sp3ctraLookAndFeel.h).
>
> Règle d'or : **aucun nombre magique** dans le code de layout/paint — tout passe par les tokens `Sp3ctraTheme::`.

---

## 1. Charte graphique

### 1.1 Palette de couleurs (thème sombre)

| Rôle | Token | Hex | Aperçu |
|------|-------|-----|--------|
| Fond principal fenêtre | `kColBg` | `#1e1e1e` | ▓ |
| Fond de panneau | `kColPanelBg` | `#282828` | ▓ |
| Surface intérieure sombre | `kColSurface` | `#1a1a2a` | ▓ |
| Bordure / séparateur | `kColBorder` | `#3a3a3a` | ▓ |
| Texte standard | `kColText` | `#b8c4d0` | ▓ |
| Texte atténué / désactivé | `kColTextMuted` | `#888888` | ▓ |
| Fond bouton | `kColBtnBg` | `#2a2a2a` | ▓ |
| Bouton actif | `kColBtnActive` | `#3a3a3a` | ▓ |

**Couleur des CONTRÔLES — « ce qu'on touche » (tokens, 2026-08-28) :**

| Rôle | Token | Hex |
|------|-------|-----|
| **Poignée / contrôle** — nœuds, chevrons, thumbs, lignes saisissables, barres (`Sp3ctraBarSlider`), toggles, combos, puces de type, knobs | `kColHandle` | `#dcff3c` (lime acide) |
| Anneau d'une poignée en cours de DRAG, bord de valeur d'une barre pressée | `kColHandleHot` | `#ffffff` |
| Cœur sombre d'une poignée au repos (= fond du cadre) | `kColHandleCore` | `#20202a` |
| Fond du cadre graphique des éditeurs | `kColFrameBg` | `#20202a` |
| Intérieur d'une barre (actif / désactivé) | `kColBarBg` / `kColBarBgOff` | `#181820` / `#121216` |

Le lime est choisi **hors des cinq teintes de catégorie** (cyan / magenta / violet / ambre / vert) : un contrôle ressort sur toutes les pages. Règle : *ce qu'on regarde* (courbes, cadres, titres, labels) = couleur du module ; *ce qu'on touche* = `kColHandle`. Recettes de peinture : [`ui/Sp3ctraHandles.h`](../vst/source/ui/Sp3ctraHandles.h) (poignées) et [`ui/ModuleEditorChrome.h`](../vst/source/ui/ModuleEditorChrome.h) (chrome d'affichage).

**Grammaire d'état d'une poignée** (une teinte, quatre états, chacun plus « chaud ») :

| État | Rendu |
|------|-------|
| Idle | creux : cœur sombre + anneau lime 1.4 px |
| Selected | cœur lime plein + anneau lime + fin anneau extérieur (sélection persistante : la poignée que pilotent les boîtes / CC MIDI) |
| Hover | cœur lime plein + halo doux |
| Drag | cœur lime plein + anneau BLANC + halo + readout lime (pilule sombre) |

Barres (`Sp3ctraBarSlider`, branche `LinearBar` du LookAndFeel) : idle = remplissage lime 22 % / liseré 35 % ; hover = remplissage 30 % / liseré 70 % / bord de valeur lime ; drag = remplissage 40 % / liseré plein / bord de valeur blanc.

**Autres couleurs fonctionnelles (hors tokens) :**

| Rôle | Hex |
|------|-----|
| Piste/arc non rempli (très sombre) | `#1a1f2a` |
| Corps de knob | `#22272f` (bordure `#33373f`) |
| Vert accent (menus : tick, section header) | `#66cc88` |
| Ambre MIDI (badge « mappé », hint temporisé) | `#e0a24a` |
| Texte de hint inactif | `#55606f` |
| Rouge PANIC | rouge vif adaptatif |
| Jaune réglages (gear) | jaune/orange adaptatif |

### 1.2 Couleurs d'identité des modules (chain rack)

Depuis 2026-08-14 un module n'a **pas** de couleur propre : sa couleur est celle de sa **catégorie** de catalogue (`moduleColour(t)` = `moduleCatColour(category)`, [`ui/ModuleCatalog.h`](../vst/source/ui/ModuleCatalog.h)). Elle est utilisée partout où le module apparaît **en tant qu'affichage** : puce catalogue, bloc du rack, barre PLAY/SETUP, bouton power, courbes / cadres / titres / labels de ses éditeurs, tranche mixeur.

| Catégorie | Hex | Couleur | Modules |
|-----------|-----|---------|---------|
| SRC | `#00d9ff` | cyan électrique | SP3CTRA, IMAGE, VIDEO, CAMERA |
| MIDI | `#ff2ed0` | magenta | PITCH, MASK |
| FX | `#b44dff` | violet néon | REVERB, ECHO, EQ, SCALE, CENTROID, LEVELS, DC BLOCK, GAIN |
| UTILS | `#ffb020` | ambre vif | SAMPLER, SCORE, TIMBRE, MIDI SCORE, VOICE |
| OUT | `#45ff8c` | vert néon | → LUXSTRAL / LUXSYNTH / LUXWAVE / LUXGRAIN, → VIDEO SCROLL, → MIDI TAP |

Jamais de teinte de module en littéral local : toujours `moduleColour(ModuleType::X)`. Les contrôles, eux, ne prennent **jamais** la couleur du module (§1.1).

### 1.2b Identité d'un paramètre (étiquette MIDI)

Partout où l'interface nomme un paramètre piloté par MIDI — lignes et bandeau replié du volet MIDI MAP, infobulles de la face CONTROLS, overlay OLED du CIS — elle passe par **une seule** description, [`ui/ParamIdentity.h`](../vst/source/ui/ParamIdentity.h) (`describeParam` + `ParamIdentityLabel::draw`) :

`CC 32  ④ VIDEO · DC BLOCK · Amount`

| Segment | Rendu |
|---------|-------|
| Événement MIDI (`CC 32`, `C#3`) | blanc `#eeeeee`, gras — jamais une teinte de chaîne ou de module (la chaîne 1 est ambre comme le badge MIDI) ; le canal n'apparaît que dans l'infobulle (`CC 32 · ch 1`) |
| Chaîne | la pastille numérotée du rack ([`ui/ChainIdentity.h`](../vst/source/ui/ChainIdentity.h) : `drawPastille`, 14 px en ligne compacte, 16 px dans le rack et la barre PLAY/SETUP) + le nom de chaîne, gras, en couleur de chaîne (`ChainIdentity::colour`) |
| Module | `moduleDisplayName` sans la flèche « → », gras, en couleur de module |
| Paramètre | le nom **nu** (`ParamNaming::bareName`) : le tag de banque (`DC2 `, `VS0 `, `LS OUT1 `…) que `moduleAbbrev` pose sur les noms APVTS pour l'unicité côté hôte est retiré, ainsi que les mots répétant le module et le numéro de slot ; texte atténué `#9aa6ba` |

Quand la place manque, le nom du paramètre s'ellipse puis disparaît, puis le module ; l'événement et la chaîne restent. Le code court d'un module (`ModuleDesc::abbrev` : `DC`, `LV`, `VS`, `LS`…) n'existe qu'à un endroit, le catalogue : il sert de tag aux noms APVTS, de code module sur l'OLED et de clé au retrait du tag.

### 1.3 Typographie

Une seule famille (police système JUCE par défaut), hiérarchie strictement par taille. Tout passe par les tokens `Sp3ctraTheme::kFont*`.

| Token | Taille | Usage |
|-------|--------|-------|
| `kFontTitle` | 22 px | Titre application (« Sp3ctra ») |
| `kFontWindowTitle` | 18 px | Titre de fenêtre flottante / dialogue |
| `kFontSection` | 16 px | En-tête de section (page settings) |
| `kFontSettings` | 14 px | Label à côté d'un contrôle (settings, combo) |
| `kFontBadge` | 12 px | Badge de section / en-tête de composant (gras) |
| `kFontBtn` | 11 px | Label de bouton |
| `kFontSmall` | 11 px | Texte auxiliaire (labels d'éditeur, knob) |
| `kFontTiny` | 10 px | Captions transport, hints séquenceur |
| `kFontMicro` | 9 px | Plus petit lisible (noms de notes dans les slots) |

### 1.4 Métriques & espacements

| Token | Valeur | Rôle |
|-------|--------|------|
| `kControlH` | 22 px | Hauteur unifiée de **tous** les contrôles |
| `kRowGap` | 4 px | Gap vertical entre rangées |
| `kRowStep` | 26 px | Pas de rangée (`kControlH + kRowGap`) |
| `kHPad` | 10 px | Marge horizontale extérieure |
| `kPad` | 4 px | Padding interne |
| `kGap` | 6 px | Gap horizontal entre éléments |
| `kLabelW` / `kLabelWide` | 110 / 140 px | Largeur colonne label |
| `kSectionH` | 24 px | Hauteur badge de section |
| `kSectionGap` | 4 px | Gap sous le badge |
| `kTbStd` / `kTbWide` / `kTbNarrow` / `kTbXNarrow` | 72 / 82 / 52 / 60 px | Largeurs de text-box de slider |
| Corner radius standard | 3–4 px | Boutons, combos, badges, panneaux |

---

## 2. LookAndFeel custom (`Sp3ctraLookAndFeel`)

Hérite de `juce::LookAndFeel_V4`. Instancié une fois dans l'éditeur, posé via `setDefaultLookAndFeel`.

| Méthode overridée | Rendu custom |
|-------------------|--------------|
| `drawButtonBackground` | Boutons-onglets (`isTab`) : fond transparent + overlay hover/press subtil. Boutons standards : rect arrondi 3 px, bordure subtile, fond adaptatif. |
| `drawButtonText` | Police uniforme `kFontBtn` (11 px), couleur selon état toggle + alpha si désactivé. |
| `createSliderTextBox` | Barres : le label superposé est texte seul (fond et liseré transparents) — le chrome de la barre est peint UNE fois, dans `drawLinearSlider`. |
| `drawLinearSlider` | **`LinearBar`** (Sp3ctraBarSlider) : intérieur `kColBarBg`, remplissage / liseré lime avec les 3 états idle · hover · drag (bord de valeur lime puis blanc). **`LinearVertical`** (faders mixeur) : même langage, barre 12 px, teinte posée par `AudioMixPanel::initFader`. `LinearHorizontal` (résiduel) : piste `#1a1f2a`, remplissage et thumb lime. |
| `drawRotarySlider` | Cadran : arc fond `#1a1f2a`, arc valeur lime 75 %, corps `#22272f`, pointeur lime, halo hover. |
| `drawToggleButton` | **Switch glissant** langage barre : piste `kColBarBg`, remplissage lime 25 % si ON, liseré lime, pastille carrée lime (OFF : grise). Accent = `ToggleButton::tickColourId` du plus proche ancêtre — `kColHandle` par défaut (PluginEditor le pose explicitement sur l'hôte zone 3). |
| `getComboBoxFont` / `drawComboBox` | Police `kFontSettings` ; intérieur `kColBarBg`, liseré lime 35 % (70 % au survol / pressé), triangle lime plein. |
| `getPopupMenuFont` / `drawPopupMenu*` | Menus sombres `kFontSmall`, header de section vert `#66cc88`, tick ✓ vert, séparateurs fins. |

---

## 3. Vocabulaire de composants (widgets de base)

| Widget JUCE | Style Sp3ctra | Où |
|-------------|---------------|-----|
| **`Sp3ctraBarSlider`** ([`ui/Sp3ctraBarSlider.h`](../vst/source/ui/Sp3ctraBarSlider.h)) | LE slider horizontal unique : barre lime, texte superposé lecture seule, double-clic = valeur par défaut, appui long = saisie au clavier, clic droit = MIDI learn (contrat unique `ui/Sp3ctraGestures.h`, partagé par les poignées des éditeurs graphiques) | **toutes** les valeurs horizontales (boîtes des éditeurs, modulation, video scroll, mixeurs) |
| `Slider` LinearVertical | fader barre 12 px, teinte moteur | AudioMixPanel |
| `Slider` Rotary | knob à cadran, arc lime | paramètres continus des panneaux moteurs (LuxSynth / LuxGrain / LuxWave) |
| `ToggleButton` | switch glissant lime | **Enable** des modules, booléens (loop, sync, velocity…) |
| `TextButton` | rect arrondi 3 px | actions (GENERATE, LOAD, REC…), transport |
| `ComboBox` | fond arrondi + triangle | sélecteurs (mode, source, note MIDI…) |
| `Label` | texte plat | labels read-only, en-têtes |
| `Viewport` | scroll vertical sobre | zones 2, 3, 4 |

### Squelette standard d'un éditeur de module (zone 3, 2026-08-28)

Tous les éditeurs FX / MIDI (DC BLOCK, GAIN, LEVELS, CENTROID, ECHO, REVERB, EQ, FILTER, ADSR…) partagent le même squelette, fourni par [`ui/ModuleEditorChrome.h`](../vst/source/ui/ModuleEditorChrome.h) (`namespace ModuleChrome`) :

```
┌ cadre (kColFrameBg, liseré module 25 %, r = 4) ─────────────────┐
│ TITRE (kFontTiny gras, module 75 %)              readout (60 %)  │
│    plot = plotOf(cadre) : courbe (module) + poignées (lime)      │
└──────────────────────────────────────────────────────────────────┘
  kRowGap 6
  Label      Label      Label        kLabelH 12 (kFontTiny, module 60 %)
  [ barre ]  [ barre ]  [ barre ]    kBoxH 18 (Sp3ctraBarSlider lime), kBoxGap 8
```

- Le cadre ne contient **que** la visu et ses poignées ; les boîtes numériques vivent dans leur rangée **sous** le cadre (`layoutBoxRow`, `drawBoxLabel`). Un éditeur à deux vues (ADSR + largeur, CENTROID) répète le motif cadre + rangée.
- Une page (`image/Lux*TabComponent.h`) empile ses éditeurs (`kEditorGap` 4, premier à `kPageTop` 4, marge `kPagePad` 8), ferme par la caption `--- NOM ---` (`drawSectionCaption`, `kSectionCaptionH` 22) et place ses rangées de contrôles supplémentaires (combos Root/Scale/Mode, Invert, Glide/LFO + toggle Velocity) dans le **même** idiome label-au-dessus. Hauteur : `pageHeight(editorsH, extraRows)`.
- Helpers : `drawFrame`, `drawCaption`, `drawReadout`, `drawBoxLabel`, `layoutBoxRow`, `drawSectionCaption`, `graphOf` / `plotOf`.
- Poignées : `Sp3ctraHandles::drawNode` (nœud plein), `drawRing` (poignée secondaire creuse : courbure, pente), `drawThumb` (thumb de fader, grip), `drawGrabLine` (ligne saisissable, ex. Floor), `drawChevron` + `drawLink` (2ᵉ réglage de l'EQ), `drawChip` (puces BELL/LP/HP/DJ/TILT), `drawReadout` (valeur pendant le drag) ; état via `stateOf(dragging, hovered, selected)`.

### Patterns de layout récurrents

- **Badge + contrôles** : badge de section (24 px, couleur accent) → bande de toggles optionnelle → grille/rangées de contrôles → gap inter-section.
- **Label + contrôle** : colonne label (80–140 px, justifié à droite) + gap + contrôle.
- **Grille de knobs** (`AudioPanelLayout`) : 4 colonnes, cellule = knob 44 px + valeur 14 px + label 13 px = 71 px.
- **Splitter + largeur persistée** : `SplitterBar` entre zones, largeurs stockées dans l'APVTS (`zone2W`, `zone4W`).

---

## 4. Architecture en 4 zones (éditeur principal)

Défini dans [`PluginEditor.h`](../vst/source/PluginEditor.h) — fenêtre redimensionnable, layout persisté dans l'APVTS.

```
┌─────────────────────────────────────────────────────────────┐  Header 52 px
│ Logo / Version / ⚙ Settings              | PANIC            │
├─────────────────────────────────────────────────────────────┤
│ ZONE 1 — CisVisualizerComponent (pleine largeur, empilé)    │  variable
│ + KeyboardRulerComponent (26 px, si PITCH/MASK)             │
├──┬──────────┬──────────────────────────────────────┬────────┤
│▌P│ ZONE 2   │ ZONE 3 — éditeur de bloc             │ ZONE 4 │
│▌A│ chain    │ FaceSwitchBar (PLAY|SETUP, 24 px)    │ video  │
│▌L│ rack     │ + viewport (pages PLAY ou SETUP)     │ scroll │
└──┴──────────┴──────────────────────────────────────┴────────┘
   Palette    SplitterBar                SplitterBar
   rail 36px
```

**Header** : `GearButton` (roue dentée jaune), `PanicButton` (rouge), `FaceSwitchBar` (PLAY/SETUP), `SettingsWindow` (fenêtre flottante).
**Sélection** : `selectedBlock` (un seul bloc) + `setupFace` (PLAY/SETUP) pilotent la zone 1 et la zone 3.
**État persisté** : `editorW/H`, `zone2W`, `zone4W`, `scrollCollapsed`.

---

## 5. Inventaire des composants custom (par zone)

### Zone 1 — Visualisation
| Composant | Fichier | Rôle |
|-----------|---------|------|
| `CisVisualizerComponent` | [`CisVisualizerComponent.h`](../vst/source/CisVisualizerComponent.h) | Vue du signal CIS spectral ~30 FPS, modes freeze/hold/white, overlay blob, panels empilés par source. |
| `BlobVisualizerComponent` | [`image/BlobVisualizerComponent.h`](../vst/source/image/BlobVisualizerComponent.h) | Vue secondaire de l'image seuillée pour la détection blob (teal + bounding boxes orange). |
| `KeyboardRulerComponent` | [`ui/KeyboardRulerComponent.h`](../vst/source/ui/KeyboardRulerComponent.h) | Règle clavier 26 px sous zone 1 (PITCH/MASK), marqueur note réf, overlay voix live ADSR. |

### Zone 2 — Chain rack
| Composant | Fichier | Rôle |
|-----------|---------|------|
| `ChainRackComponent` | [`ui/ChainRackComponent.h`](../vst/source/ui/ChainRackComponent.h) | Liste verticale de blocs (CHAIN 1 / CHAIN 2), LED d'état 10 Hz, sélection, bouton swap ⇅. |
| `PaletteRailComponent` | [`ui/PaletteRailComponent.h`](../vst/source/ui/PaletteRailComponent.h) | Rail gauche 36 px (SRC/FX/OUT) — stub, drag-drop prévu M6. |
| `SplitterBar` | [`ui/SplitterBar.h`](../vst/source/ui/SplitterBar.h) | Diviseur vertical draggable, 3 grip dots, largeur persistée. |

### Zone 3 — Pages PLAY (édition de bloc)
| Composant | Fichier | Rôle |
|-----------|---------|------|
| `SourcesTabComponent` | [`image/SourcesTabComponent.h`](../vst/source/image/SourcesTabComponent.h) | Transport de la source (Play/Hold/Stop, Fade In). |
| `LuxPitchTabComponent` | [`image/LuxPitchTabComponent.h`](../vst/source/image/LuxPitchTabComponent.h) | Page PITCH : **éditeur ADSR graphique** + modulation (glide, LFO, velocity). |
| `LuxMaskTabComponent` | [`image/LuxMaskTabComponent.h`](../vst/source/image/LuxMaskTabComponent.h) | Page MASK : éditeur ADSR + éditeur de filtre interactif + modulation. |
| `LuxStralTabComponent` / `LuxSynthTabComponent` | [`image/LuxStralTabComponent.h`](../vst/source/image/LuxStralTabComponent.h) | Réglages image (négatif, gamma, contraste, détection blob). |
| `ScoreGenTabComponent` | [`image/ScoreGenTabComponent.h`](../vst/source/image/ScoreGenTabComponent.h) | Génération spectrogramme (Load WAV, Writing Speed, GENERATE, Export, transport). |
| `AudioStralPanel` / `AudioSynthPanel` / `AudioWavePanel` | [`ui/EngineAudioPanels.h`](../vst/source/ui/EngineAudioPanels.h) | Paramètres audio des moteurs (volume = knob, ADSR = éditeur de courbe, filtre/LFO = knobs). |

### Zone 3 — Faces SETUP
| Composant | Fichier | Rôle |
|-----------|---------|------|
| `SetupHeader` (helper) | [`ui/setup/SetupHeader.h`](../vst/source/ui/setup/SetupHeader.h) | En-tête SETUP uniforme (titre couleur accent + underline). |
| `PitchSetupPanel` / `MaskSetupPanel` | [`ui/setup/PitchSetupPanel.h`](../vst/source/ui/setup/PitchSetupPanel.h) | Réglages MIDI (canal, octave, note réf, polyphonie) + mapping step. |
| `LuxStralSetupPanel` / `LuxSynthSetupPanel` / `LuxWaveSetupPanel` | [`ui/setup/`](../vst/source/ui/setup/) | Réglages spécifiques moteur (tuning, octaves, soft-limit…). |
| `SamplerSetupPanel` / `ScoreSetupPanel` | [`ui/setup/SamplerSetupPanel.h`](../vst/source/ui/setup/SamplerSetupPanel.h) | Réglages sampler / génération de partition. |

### Sampler
| Composant | Fichier | Rôle |
|-----------|---------|------|
| `SamplerPageComponent` | [`sampler/SamplerPageComponent.h`](../vst/source/sampler/SamplerPageComponent.h) | Conteneur : grille slots + éditeur + séquenceur + transport. |
| `SlotGridComponent` | [`sampler/SlotGridComponent.h`](../vst/source/sampler/SlotGridComponent.h) | 12 slots (C1..B1), couleur d'état, clignement 10 Hz. |
| `SlotEditorComponent` | [`sampler/SlotEditorComponent.h`](../vst/source/sampler/SlotEditorComponent.h) | Édition du slot : REC/PLAY/CLEAR/SAVE/LOAD, timeline, speed, loop, courbe. |
| `SlotTimelineComponent` | [`sampler/SlotTimelineComponent.h`](../vst/source/sampler/SlotTimelineComponent.h) | Timeline spectrale symétrique + handles (start/end/attack/decay/cuts). |
| `SequencerComponent` | [`sampler/SequencerComponent.h`](../vst/source/sampler/SequencerComponent.h) | Grille 16 steps (2×8), thumbnails spectraux, clic = banque. |
| `TransportBarComponent` | [`sampler/TransportBarComponent.h`](../vst/source/sampler/TransportBarComponent.h) | BPM, steps, loop, DAW sync, Play/Hold/Stop. |

### Zone 4 — Waterfall / Video scroll
| Composant | Fichier | Rôle |
|-----------|---------|------|
| `WaterfallColumnComponent` | [`ui/WaterfallColumnComponent.h`](../vst/source/ui/WaterfallColumnComponent.h) | Colonne repliable, mini-header (détacher/replier), toolbar display. |
| `VideoScrollPage` | [`video/VideoScrollPage.h`](../vst/source/video/VideoScrollPage.h) | Page zone 3 d'une sortie VIDEO SCROLL, squelette `ModuleChrome` : cadre VIEWPORT + rangées Rotation · Zoom · Center X/Y · Line Pos / Speed · Thickness · Compression · Fade · Blur · Gamma / Invert · Color · Background. |
| `VideoScrollViewportEditor` | [`video/VideoScrollViewportEditor.h`](../vst/source/video/VideoScrollViewportEditor.h) | Pad VIEWPORT (2026-08-28) : la fenêtre de sortie à son vrai rapport, vignette live du VIDEO MIX, cadre de zoom saisissable — glisser = Center X/Y, coin = Zoom, levier / cadran = Rotation, ligne de naissance = Line Pos, pincement ou ⌘+molette = Zoom, double-clic = reset, clic droit = MIDI Learn. Affichage = couleur module, poignées = lime. |
| `VideoWindow` | [`video/VideoWindow.h`](../vst/source/video/VideoWindow.h) | Fenêtre waterfall détachée. |

### Settings (fenêtre flottante)
| Composant | Fichier | Rôle |
|-----------|---------|------|
| `SettingsWindow` / `SettingsComponent` | [`SettingsWindow.h`](../vst/source/SettingsWindow.h) | Fenêtre réglages machine (tabs Network / System). |
| `NetworkSettingsTab` / `SystemSettingsTab` | [`settings/`](../vst/source/settings/) | UDP + DPI capteur / log, threads LuxStral, taille fenêtre vidéo détachée. |

### Boutons custom du header (dans `PluginEditor.h`)
| Élément | Rendu |
|---------|-------|
| `GearButton` | Roue dentée 8 dents jaune, trou central, animée au survol. |
| `PanicButton` | Rect rouge bordé, texte « PANIC » blanc gras. |
| `FaceSwitchBar` | 2 segments PLAY/SETUP, segment actif surligné couleur accent. |

---

## 6. Éléments dessinés à la main (`paint()`)

Composants à rendu vectoriel custom (graphes, courbes, glyphes) :

| Composant | Éléments peints |
|-----------|-----------------|
| `EnvelopeEditorComponent` | Courbe ADSR shaped par segment (bend), nodes A/D/S/R, handles de courbure, lane width (MASK). |
| `MaskFilterEditorComponent` | Courbe passe-bande, remplissage live « breathing », handles de bords + slope, marqueur note. |
| `KeyboardRulerComponent` | Touches piano, labels octave, marqueur note réf, overlays voix. |
| `CisVisualizerComponent` / `BlobVisualizerComponent` | Image CIS rasterisée, bounding boxes blob. |
| `SlotTimelineComponent` | Thumbnail spectral symétrique (gamma 0.4), barres start/end, triangles attack/decay, cuts. |
| `SlotGridComponent` / `SequencerComponent` | Cellules d'état colorées, thumbnails spectraux. |
| `ChainRackComponent` | Headers de chaîne, fonds de bloc, LED d'état, flèches de connexion. |
| `WaterfallColumnComponent` | Glyphes path (détacher/replier/déplier), toolbar. |
| `FaceSwitchBar` / `SplitterBar` / `PaletteRailComponent` | Segments, grip dots, badges catégorie. |
| `EngineAudioPanels` | Badges de section, fonds de section, labels & valeurs de knobs. |

---

## 7. Conventions de design

1. **Tokens d'abord** — jamais de littéral de couleur/taille/espacement hors `UITheme.h` (sauf couleurs fonctionnelles documentées §1.1).
2. **Couleur = identité de catégorie pour l'AFFICHAGE, lime pour les CONTRÔLES** — un module garde sa couleur (rack, onglet, en-tête, courbes, cadres, labels) ; tout ce qui se saisit (poignées, barres, toggles, combos, puces, knobs) est `kColHandle`, avec les états idle / selected / hover / drag de `Sp3ctraHandles`.
3. **Hauteur de contrôle unique** — `kControlH` (22 px) pour boutons, sliders, combos → alignement parfait des rangées.
4. **Choix du contrôle selon la nature du paramètre** :
   - valeur numérique d'un module (temps, dB, %, Hz…) → **`Sp3ctraBarSlider`** dans la rangée sous le cadre ;
   - forme / enveloppe / courbe → **éditeur graphique** (poignées lime `Sp3ctraHandles` + boîtes en dessous) ;
   - continu « moteur » (panneaux LuxSynth / LuxGrain / LuxWave) → **knob rotatif** ;
   - booléen / activation → **toggle switch** ;
   - discret/énuméré → **combo**, dans une rangée label-au-dessus comme les barres.
5. **Rendu sombre cohérent** — fonds `#1e1e1e`/`#282828`, couleur de catégorie pour l'affichage, lime pour les contrôles, ambre pour la navigation et le MIDI.
6. **Squelette unique des pages de module** — `ModuleChrome` (cadre, titre, rangée de boîtes, caption de section) ; aucune page ne redessine son propre cadre, sa propre caption `--- X ---` ni sa propre recette de poignée.

---

*Document généré dans le cadre de la refonte UI (sliders → knobs/éditeurs) ; mis à jour le 2026-08-28 (couleur de contrôle lime + squelette `ModuleChrome` / `Sp3ctraHandles`). À tenir à jour lors de l'ajout de nouveaux composants ou tokens.*
