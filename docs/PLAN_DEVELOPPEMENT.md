# Sp3ctra — Plan de développement : architecture modulaire & jouabilité

> Version 1.0 — 2026-06-12
> Synthèse des sessions de conception sur Pitch/Mask, le pipeline image modulaire,
> le layout 4 zones et la dual-instance LuxStral.

---

## 1. Vision

Sp3ctra devient un **instrument semi-modulaire synesthésique** : l'utilisateur
construit jusqu'à **deux chaînes d'image** (source → effets → sorties), voit en
permanence ce qu'il entend, et joue l'image au MIDI avec une expressivité de
synthétiseur (vélocité, sustain, molette, glide, polyphonie).

Principes non négociables :

1. **On voit ce qu'on entend** — tout traitement qui affecte l'audio est visible
   dans la chaîne et dans les vues.
2. **Toucher un maillon = tout l'écran en parle** — un modèle de sélection
   unique pilote vues, éditeur et waterfall (avec épinglage 📌 par zone).
3. **Linéaire, pas patchable librement** — chaînes = listes ordonnées.
   Deux chaînes maximum. Pas de graphe libre.
4. **Les réglages d'un module vivent dans le module** — la roue dentée ne garde
   que l'infrastructure (réseau, logs, threads).
5. **RT-safe partout** — reconfiguration de chaîne par swap atomique entre
   frames + crossfade ~80 ms ; aucun changement de topologie en cours de frame.

---

## 2. Architecture cible

### 2.1 Modèle de blocs

```c
/* Trois contrats */
Source : void                  → RGB(n)     /* produit des frames    */
Effet  : RGB(n)                → RGB(n)     /* transforme            */
Sortie : RGB(n)                → ∅          /* consomme, pass-through */
```

| Catégorie | Blocs | Multiplicité |
|---|---|---|
| **Sources** | Sp3ctra (CIS UDP) · Image fixe · Séquenceur d'images · Sampler · Tap (sortie ▣ de l'autre chaîne) | 1 par chaîne |
| **Effets** | Pitch · Mask · Tone (contraste+inversion+AC+gamma groupés, bypass individuels internes) · EQ graphique spatial · Mixeur (receive) · Blob | 1 par type **et par chaîne** (params pré-déclarés ×2) |
| **Sorties** | ♪ LuxStral (×2 après M8) · ♪ LuxSynth (×1) · ♪ LuxWave (×1) · ▣ Video Out (tap nommé) | moteurs limités par leurs singletons ; ▣ illimités |

### 2.2 Règles de routage

- **Taps** : la sortie de *chaque* bloc est visualisable (sélection). Seuls les
  blocs ▣ Video Out sont **routables** : consommables par le waterfall épinglé,
  par la source « Tap » de l'autre chaîne, et par l'entrée d'enregistrement du
  Sampler.
- **Anti-cycle** (2 chaînes) : une chaîne ne peut pas consommer un tap d'une
  chaîne qui la consomme déjà. Vérification statique au branchement, refus avec
  message clair. Ordre d'exécution déduit (la chaîne consommée s'exécute d'abord).
- **Sampler = source** dont l'entrée d'enregistrement est un sélecteur de tap
  (n'importe quel ▣). Résout « échantillonner après Pitch » sans sampler-insert.
- **Moteurs** : les adaptateurs internes non créatifs (conversion gris,
  moyennage par note, FFT) restent dans le moteur. Tout le reste (Tone, Blob)
  devient bloc de chaîne — donc **visible**.

### 2.3 Paramètres & persistance

- Les **paramètres** des blocs restent des params APVTS nommés, pré-déclarés
  par (type × chaîne) pour les effets, par instance pour les moteurs
  (`luxstralB*` au jalon M8). Automation DAW propre, IDs stables.
- La **topologie** (quels blocs, quel ordre, quelle chaîne, branchements de
  taps) vit dans le **ValueTree de session**, pas en paramètres.
- **Migration** : au premier chargement d'une session ancienne, génération de
  la topologie équivalente à l'actuelle (voir conflit C1 pour le détail Sampler).

---

## 3. Interface cible — layout 4 zones (+1 réservée)

Fenêtre **redimensionnable** (défaut ~1280×820, min ~1024×700, taille persistée).

```
┌──────────────────────────────────────────────────────────────────────┐
│ Header                                                           ⚙   │
├──────────────────────────────────────────────────────────────────────┤
│ ZONE 1 — VUES pleine largeur fenêtre : ligne IN / OUT du bloc        │
│          sélectionné (une seule bande si source)                     │
├──┬─────────────┬──────────────────────────────────────┬──────────────┤
│▌ │ ZONE 2      │ ZONE 3 — ÉDITEUR (scroll ↕)          │ ZONE 4       │
│▌P│ CHAÎNES 1&2 │ onglets PLAY | SETUP                 │ VIDEO SCROLL │
│▌A│ (scroll ↕)  │ éditeurs graphiques :                │ masquable ▶  │
│▌L│ flux ↓      │ ADSR dessinable, clavier-règle,      │ 📌 ⤢ ⧉      │
│▌ │ LED ●◐○     │ courbes Tone/EQ…                     │ (détachable) │
├──┴─────────────┴──────────────────────────────────────┴──────────────┤
│ ZONE 5 (réservée, repliée h=0) — sortie / master / monitoring        │
└──────────────────────────────────────────────────────────────────────┘
```

- **Palette** (rail ~36 px replié / ~200 px déplié) : 3 catégories
  Sources / Effets / Sorties ; blocs non disponibles grisés (moteur déjà placé,
  effet déjà présent dans la chaîne).
- **Zone 2** : deux chaînes empilées, flux haut→bas, LED 3 états
  (● actif / ◐ auto-bypass / ○ off), `[+]` insertion, ↑↓ déplacement
  (drag-and-drop en V2), clic droit = suppression. Les moteurs ♪ terminent les
  chaînes ; les ▣ portent leur nom de tap.
- **Zone 3** : chaque éditeur a deux faces — **PLAY** (contrôles de jeu) et
  **SETUP** (canal MIDI, octave, note de référence, polyphonie, export…).
- **Zone 4** : waterfall vertical (hauteur = historique temporel), suit la
  sélection sauf 📌 ; sous-échantillonné horizontalement ; ⧉ fenêtre détachée
  (`juce::DocumentWindow`) livrée en même temps ; ⤢ plein écran.
- **Priorités d'effondrement** en fenêtre étroite : zone 4 d'abord (repli en
  bouton), puis palette ; la zone 3 a priorité. Splitters persistés.
- Les **onglets actuels disparaissent** au jalon M4 (pas de double navigation) ;
  l'onglet VIDEO devient zone 4/fenêtre détachée ; les sous-onglets Audio*
  deviennent les éditeurs des blocs ♪.

---

## 4. Conflits logiques relevés et tranchés

| # | Conflit | Décision |
|---|---|---|
| **C1** | **Ordre Sampler incohérent** : les commentaires UI et `image_pipeline_types.h` annoncent `Live ► Sampler ► Pitch ► Mask`, mais `multithreading.c` implémente `Live ► Pitch ► Mask ► Sampler(rec)` avec relecture court-circuitant les inserts. | Le modèle cible « **Sampler = source à entrée d'enregistrement sélectionnable (tap)** » supprime l'ambiguïté. Migration : sessions actuelles → Chaîne 1 = `Sampler(rec: tap Video A) → … ` reproduisant le comportement « imprimé » actuel. Corriger les commentaires dès M1. |
| **C2** | Si tout bloc est visualisable, à quoi servent les ▣ Video Out ? | Visualiser ≠ router. Tout bloc se *regarde* (sélection) ; seuls les ▣ se *branchent* (inter-chaîne, sampler-rec, waterfall épinglé). |
| **C3** | Vues « pleine largeur » vs colonnes latérales (palette/scroll). | Zone 1 au-dessus de **tout**, pleine largeur fenêtre. Les colonnes ne commencent qu'en dessous. |
| **C4** | Préprocessing par moteur (`luxstralGamma` ≠ `luxsynthGamma`) vs bloc Tone par chaîne. | Tone est **par chaîne** : deux moteurs sur la même chaîne partagent le préprocessing ; un préprocessing différent = deux chaînes. Migration des params vers `tone1*`/`tone2*`. |
| **C5** | « Un exemplaire par type » vs Tone/EQ nécessaires dans chaque chaîne. | Effets : **1 instance par type et par chaîne** (params ×2 pré-déclarés). Pitch/Mask : ×1 global en M1 (extension ×2 si besoin avéré). Moteurs : selon singletons (M8 pour LuxStral ×2). |
| **C6** | Note de référence « A3 = centre fixe » (décision utilisateur) vs param existant. | A3 reste le **défaut** ; le param survit en face SETUP pour les cas particuliers. |
| **C7** | Boucles inter-chaînes via source Tap + Mixeur receive. | Règle anti-cycle statique au branchement (§2.2). |
| **C8** | Triple simulation (`g_*`, `_proc`, `_vid`) vs multiplication des vues. | **Snapshot unique** (M2) : `_proc` fait foi, les vues dessinent un snapshot publié par frame. Supprime aussi la triple duplication du dispatch MIDI dans `PluginProcessor.cpp`. |
| **C9** | Settings éparpillés (9 onglets roue dentée). | Redistribution intégrale vers les faces SETUP des blocs (M5). La roue dentée ne garde que : réseau (port/IP/DPI), log level, workers. |
| **C10** | Largeur waterfall (3456 px → ~300 px de colonne). | Assumé pour le monitoring de mouvement ; le **détail** passe par la fenêtre détachée ⧉, livrée avec la zone 4, pas après. |

---

## 5. Jalons

### M0 — Quick wins DSP musicaux *(indépendant de tout, livrable immédiatement)*
1. **Interpolation sous-pixel** dans LuxPitch (lerp entre pixels voisins) —
   glide/vibrato/bend continus au lieu de sauts spectraux.
2. **Retrigger au vol de voix** : flag posé par `note_on` quand la voix volée
   était active → ré-ATTACK propre (Pitch + Mask).
3. **Pédale de sustain CC64** : note-offs différés tant que la pédale est
   enfoncée (Pitch + Mask).
4. **CC1 (molette) → profondeur de vibrato** additive sur le LFO de position.
5. Skew logarithmique sur les paramètres temporels (si manquant).

**Acceptation** : non-régression à pédale/molette neutres ; glide LuxPitch
visuellement continu ; pas de téléport au vol de voix.

### M1 — Cœur chaîne dynamique *(socle de tout le reste)*
- Contrat `ImageBlock` (source/effet/sortie) ; wrapping de Pitch, Mask,
  Sampler, CIS dans le contrat.
- Exécuteur en boucle dans `multithreading.c` remplaçant les appels codés en
  dur ; descripteur de chaîne swappé atomiquement entre frames + crossfade.
- Topologie en ValueTree + migration automatique des sessions existantes.
- Correction des commentaires C1.

**Acceptation** : sortie audio/vidéo bit-identique à l'existant sur la
topologie par défaut ; permutation Pitch↔Mask fonctionnelle (premier bénéfice
visible).

### M2 — Snapshot d'état unique
- Suppression de `g_lux_pitch`/`g_lux_pitch_vid` et équivalents Mask ;
  `_proc` fait foi ; publication par frame (par voix : note, position, largeur,
  niveau d'enveloppe) en double-buffer lock-free.
- Le dispatch MIDI de `PluginProcessor.cpp` ne cible plus qu'une instance.

**Acceptation** : visuel == audio (une seule simulation) ; CPU UI en baisse.

### M3 — Dé-globalisation LuxStral (phase A)
- Tout l'état global (`g_rt_*`, barrières, freeze, mutex, buffers) entre dans
  une struct `LuxStralEngine` ; fonctions à pointeur d'instance ; une seule
  instance créée. Pool de workers rattaché à l'instance.

**Acceptation** : non-régression stricte (A/B avec l'existant) ; aucun
changement UX.

### M4 — Coquille UI 4 zones
- Fenêtre redimensionnable + splitters persistés.
- Zone 1 (vues IN/OUT), zone 2 (chaînes, LED, +/↑↓/suppr.), zone 3 (éditeur
  contextuel, recyclant les composants actuels en l'état), zone 4 (waterfall
  colonne + 📌 + ⤢ + ⧉ fenêtre détachée), zone 5 réservée repliée.
- Palette escamotable (rail/déplié). Modèle de sélection unifié.
- Retrait des onglets.

**Acceptation** : toutes les fonctions actuelles accessibles dans le nouveau
layout ; scroll détachable ; aucune fonctionnalité orpheline.

### M5 — Éditeurs de modules PLAY/SETUP + migration settings
- Pattern d'éditeur à deux faces ; redistribution des 9 onglets de settings
  (cf. C9) — IDs APVTS inchangés.
- Éditeurs graphiques musicaux, dans l'ordre : **Pitch, Mask** (ADSR
  dessinable, clavier-règle alignée au mapping px/demi-ton, voix actives
  affichées), puis moteurs ♪, puis Sampler (slots visuels), puis le reste.

**Acceptation** : roue dentée réduite à réseau/log/workers ; Pitch et Mask
éditables sans lire la doc.

### M6 — Nouveaux blocs effets & taps routables
- Bloc **Tone** (groupé, migration C4), **EQ graphique** (courbe de gain
  dessinable), **Mixeur** (receive d'un ▣, modes add/mult/min/max/xfade),
  ▣ **Video Out** routables, **Sampler record-tap** (C1).
- Règle anti-cycle (C7) appliquée à l'UI de branchement.

**Acceptation** : ajout/suppression/déplacement de blocs sans clic audio
(crossfade) ; échantillonner la sortie de Pitch fonctionne.

### M7 — Nouvelles sources
- **Image fixe** (PNG/JPG : ligne choisie ou balayage vertical à vitesse
  réglable), **Séquenceur d'images** (pas-à-pas sur slots, sync tempo hôte —
  conception dédiée : quantification, direction, probabilité).

**Acceptation** : une session jouable sans capteur CIS branché.

### M8 — LuxStral ×2 (phases B & C)
- Banque `luxstralB*` pré-déclarée ; second bloc ♪LuxStral dans la palette ;
  **pool de workers partagé** (jobs tagués par instance) ; paramètre de
  résolution (pixels/note) **par instance** comme garde-fou CPU.

**Acceptation** : 2 LuxStral simultanés, images/tunings différents, CPU borné
par les réglages de résolution.

### M9 — Expressivité avancée *(continu, après M5)*
- Aftertouch → largeur Mask ; CC11 → gain d'alpha ; courbes de vélocité ;
  LFO sync tempo + phase par voix/key-sync ; splits de clavier (plages de
  notes par module) ; MPE ; « Focus » de forme du masque ; magnétisme de
  contenu (attraction vers maxima de luminance) ; presets de geste.

---

## 6. Dépendances

```
M0 ──────────────────────────────► (indépendant)
M1 ──► M4 ──► M5 ──► M9
M2 ──► M4 (vues fiables)        M5 ──► M8 (settings par bloc avant dual)
M3 ──► M8                       M1 ──► M6 ──► M7
```

Parallélisables : M0+M1, M2+M3, M6+M7 (équipes/threads de travail distincts).

## 7. Risques principaux

| Risque | Mitigation |
|---|---|
| Refactor M3 volumineux (état global LuxStral) | A/B systématique contre binaire de référence ; aucune feature mélangée au refactor |
| Régression sessions (topologie ValueTree) | Suite de sessions de test ; migration auto testée sur les presets existants |
| CPU dual-LuxStral | Résolution par instance (M8-C) obligatoire avant ouverture de la feature |
| Largeur minimale du layout | Priorités d'effondrement définies (C10/§3) dès la conception, pas après |
| Clics audio à la reconfiguration de chaîne | Crossfade 80 ms systématique dans l'exécuteur (M1) |

## 8. Annexe — Redistribution des settings (C9)

| Origine (roue dentée) | Destination |
|---|---|
| LuxStralSettingsTab (tuning, root, octaves, physio, soft-limit, contrast, enable) | SETUP du bloc ♪LuxStral (sauf workers → System) |
| LuxPitch/LuxMaskSettingsTab (canal, octave, réf., polyphonie) | SETUP des blocs Pitch / Mask |
| LuxSynth/LuxWaveSettingsTab | SETUP des blocs ♪ correspondants |
| LuxSamplerSettingsTab (durée, export, slots, bindings, dossier) | SETUP du bloc Sampler |
| VideoScrollSettingsTab (brightness, invert, color, tailles) | Barre d'outils du panneau scroll |
| GeneralSettingsTab (visualizer mode) | Obsolète (modèle de sélection M4) |
| **Reste en roue dentée** | Réseau (port/IP/DPI), log level, workers |
