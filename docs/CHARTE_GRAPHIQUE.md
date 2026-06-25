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

**Couleurs fonctionnelles (hors tokens, codées dans les composants) :**

| Rôle | Hex |
|------|-----|
| Accent bleu (slider/knob rempli, valeur) | `#4fa3e0` |
| Pointeur / thumb bleu pâle | `#a0c4e8` |
| Piste/arc non rempli (très sombre) | `#1a1f2a` |
| Corps de knob | `#22272f` (bordure `#33373f`) |
| Vert accent (menus : tick, section header) | `#66cc88` |
| Rouge PANIC | rouge vif adaptatif |
| Jaune réglages (gear) | jaune/orange adaptatif |

### 1.2 Couleurs d'identité des blocs (chain rack)

Chaque bloc possède une couleur d'accent utilisée **partout** où il apparaît (fond du bloc, surbrillance d'onglet, en-tête SETUP, accent des graphes/éditeurs).

| Bloc | Hex | Couleur |
|------|-----|---------|
| SOURCE CIS (chain 1 & 2) | `#68788f` | gris-bleu |
| PITCH | `#e06bb8` | rose |
| MASK | `#6be0d0` | teal |
| SAMPLER | `#4fa3e0` | bleu |
| SCORE | `#e0a24a` | ambre |
| LUXSTRAL | `#6b4fa3` | violet |
| LUXSYNTH | `#e08844` | orange |
| LUXWAVE | `#4ae0a0` | vert |

**Accents de sous-onglets (héritage IMAGE)** : Sources `#7aade0`, LuxStral `#4fa3e0`, LuxSynth `#e08844`.
**Accent underline onglet actif** : `kColTabAccent` = `#c89650` (ambre chaud).

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
| `drawLinearSlider` | Horizontal uniquement : piste sombre `#1a1f2a`, remplissage `#4fa3e0`, thumb circulaire `#a0c4e8` + halo hover. Autres styles → délégués à V4. |
| `drawRotarySlider` | Cadran : arc fond `#1a1f2a`, arc valeur `#4fa3e0`, corps `#22272f`, pointeur `#a0c4e8`, halo hover. |
| `drawToggleButton` | **Switch type iOS** : piste arrondie (off `#33373f` / on `#4fa3e0`), pastille blanche glissante, label à droite police `kFontBtn`. |
| `getComboBoxFont` / `drawComboBox` | Police `kFontSettings`, fond arrondi + petit triangle blanc plein (remplace le chevron Unicode JUCE). |
| `getPopupMenuFont` / `drawPopupMenu*` | Menus sombres `kFontSmall`, header de section vert `#66cc88`, tick ✓ vert, séparateurs fins. |

---

## 3. Vocabulaire de composants (widgets de base)

| Widget JUCE | Style Sp3ctra | Où |
|-------------|---------------|-----|
| `Slider` LinearHorizontal | piste + thumb accent | sliders spatiaux/bipolaires (video scroll), settings |
| `Slider` LinearBar | boîte numérique compacte | boîtes A/D/S/R des éditeurs de courbe, filtre masque |
| `Slider` Rotary | knob à cadran | **volumes & paramètres continus** des panneaux audio |
| `ToggleButton` | switch iOS | **Enable** des modules, booléens (loop, sync, velocity…) |
| `TextButton` | rect arrondi 3 px | actions (GENERATE, LOAD, REC…), transport |
| `ComboBox` | fond arrondi + triangle | sélecteurs (mode, source, note MIDI…) |
| `Label` | texte plat | labels read-only, en-têtes |
| `Viewport` | scroll vertical sobre | zones 2, 3, 4 |

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
| `VideoScrollTab` | [`video/VideoScrollTab.h`](../vst/source/video/VideoScrollTab.h) | Contrôles waterfall : speed (bipolaire), line pos, thickness, zoom, fade, compression. |
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
2. **Couleur = identité de bloc** — un module garde sa couleur d'accent partout (rack, onglet, en-tête, graphe).
3. **Hauteur de contrôle unique** — `kControlH` (22 px) pour boutons, sliders, combos → alignement parfait des rangées.
4. **Choix du contrôle selon la nature du paramètre** :
   - continu « audio » (volume, gain, cutoff, depth) → **knob rotatif** ;
   - enveloppe temporelle (ADSR) → **éditeur de courbe** (nodes draggables + boîtes numériques) ;
   - bipolaire/spatial (position, vitesse −1..+1) → **slider linéaire** ;
   - booléen / activation → **toggle switch** ;
   - discret/énuméré → **combo** (ou stepper).
5. **Rendu sombre cohérent** — fonds `#1e1e1e`/`#282828`, accents froids (bleu) pour les valeurs, accents chauds (ambre) pour la navigation.

---

*Document généré dans le cadre de la refonte UI (sliders → knobs/éditeurs). À tenir à jour lors de l'ajout de nouveaux composants ou tokens.*
