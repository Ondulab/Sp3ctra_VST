# PLAN — Le fond (background) devient une propriété de la CHAÎNE

**Date : 2026-08-21 — Chantier « chain porteuse des réglages », volet BACKGROUND (suite de J1–J4)**

> **RÉALISÉ le 2026-08-21** — B1–B5 implémentés et vérifiés end-to-end
> (badge rack, propagation, migration session schéma 3, persistance
> quit/relance, pages modules sans combo Background). Écart au plan : les
> membres `processor` devenus inutiles dans 6 TabComponents ont été retirés
> (warnings), et le badge est autorisé même en mode verrouillé (geste de
> performance). Ce document reste la référence de conception.

## 1. Contexte et objectif

Aujourd'hui, 11 types de modules traitent le flux différemment selon que le fond
est blanc ou noir, et chacun porte SON propre paramètre `BackgroundMode` par
banque d'instance (8 banques par type). Quand l'utilisateur change de support
(papier blanc ↔ écran noir), il doit changer le paramètre sur chaque module de
la chaîne, un par un.

Objectif : le fond devient une propriété **de la chaîne**. Un seul réglage par
chaîne, édité dans le rack, propagé à tous les modules membres. Les sélecteurs
« Background » disparaissent des éditeurs de modules.

Ce chantier était déjà anticipé (note LEVELS du 2026-08-16 : « fond blanc/noir
propriété de chaîne ») et s'inscrit dans le chantier existant « chain porteuse
des réglages » (J1 manifest → J4 presets .sp3chain).

## 2. État des lieux (inventaire)

### 2.1 Les 11 types de modules concernés

| Type | Banque APVTS | Choix actuels | Défaut | Détecteur AUTO |
|---|---|---|---|---|
| LuxPitch | `luxpitch{0..7}_BackgroundMode` | {Black, White} | White | non |
| LuxMask | `luxmask{0..7}_BackgroundMode` | {Black, White} | White | non |
| LuxReverb | `luxreverb{0..7}_BackgroundMode` | {Auto, Black, White} | White | oui |
| LuxEcho | `luxecho{0..7}_BackgroundMode` | {Auto, Black, White} | White | oui |
| LuxEq | `luxeq{0..7}_BackgroundMode` | {Auto, Black, White} | White | oui |
| LuxHarmo (SCALE) | `luxharmo{0..7}_BackgroundMode` | {Auto, Black, White} | White | oui |
| LuxCentro | `luxcentro{0..7}_BackgroundMode` | {Auto, Black, White} | White | oui |
| LuxDrive (LEVELS) | `luxdrive{0..7}_BackgroundMode` | {Auto, Black, White} | White | oui |
| LuxDcBlock | `luxdcblock{0..7}_BackgroundMode` | {Auto, Black, White} | White | oui |
| LuxGain | `luxgain{0..7}_BackgroundMode` | {Auto, Black, White} | White | oui |
| MIDI TAP | `midiTap{0..7}_backgroundMode` | {Black, White, Auto} | **Auto** | oui |

Soit 88 paramètres APVTS de portée module à retirer. Attention aux **trois
ordres de choix différents** (FX : Auto=0 ; MidiTap : Auto=2 ; Pitch/Mask sans
Auto) — la migration doit normaliser par famille.

Côté C, chaque module stocke `config.background_mode` (convention commune
`BG_BLACK=0 / BG_WHITE=1 / BG_AUTO=2`, ex. `processing/lux_drive.h:108-110`)
et, pour les 9 types avec AUTO, un état d'apprentissage verrouillé après une
fenêtre (`auto_bg_white`, `auto_locked`, …). Les modules gèrent déjà le
changement de mode à chaud (ex. `lux_eq.c:259` : reset de LUT quand
`last_bg_mode` change) — **rien à changer dans les .c des 9 FX**.

### 2.2 Les sites de code actuels

- **Création des paramètres** : `PluginProcessor.cpp` lignes ~799, 877, 972,
  995, 1022, 1062, 1109, 1175, 1194, 1213 (+ ~1895 pour MidiTap).
- **Synchro APVTS → config C** : `applyConfigurationToCore()`
  (`PluginProcessor.cpp:7880`), blocs par type lignes ~8119-8447, chacun avec
  sa table de remap (`kRvBgChoiceToMode`…). Chemin rapide topologie :
  `deriveChainRouting()` L5776-5852 (écrit `config.enabled` directement).
- **Manifest** : `ui/ModuleParamManifest.h` — suffixe `"BackgroundMode"` dans
  kPitch (L249), kMask (L257), kReverb (L266), kEcho (L269), kEq (L275),
  kHarmo (L280), kCentro (L287), kDrive (L294), kDcBlock (L297), kGain (L300),
  et `"backgroundMode"` dans kMidiTap (L319).
- **UI modules** : combo + MidiLearnAttachment dans les 10 TabComponents
  (`image/LuxPitchTabComponent.h:94`, `LuxMaskTabComponent.h:111`,
  `LuxReverbTabComponent.h:59`, `LuxEchoTabComponent.h:59`,
  `LuxEqTabComponent.h:61`, `LuxHarmoTabComponent.h:107`,
  `LuxCentroTabComponent.h:85`, `LuxDriveTabComponent.h:96`,
  `LuxDcBlockTabComponent.h:58`, `LuxGainTabComponent.h:57`) + ligne COMMON
  de `midi/MidiTapPage.h:233`.

### 2.3 Infrastructure de chaîne (où accrocher le nouveau réglage)

- **Modèle** : `ui/ChainModel.h:47-58` — `struct Chain { id; modules;
  typeMemory; }`. Aucune autre propriété de chaîne aujourd'hui. Sérialisation
  `ChainModel.cpp:378-465` (nœud `CHAIN`), `kSchemaVersion = 3`.
- **Aucun paramètre APVTS de portée chaîne n'existe** (précédent :
  `chainInsertOrder` a été supprimé, pas conservé).
- **Compilation modèle → RT** : `deriveAndPublishChainPlan()`
  (`PluginProcessor.cpp:6484-6851`) ; les instances poolées sont résolues par
  `poolSlotForInstance(uuid)` (mapping UUID → slot de pool, **à sens unique**) ;
  MidiTap/VideoScroll/sends utilisent `mi.slot` du modèle.
- **Persistance** : `captureFullState()` (L4095) resnapshote CHAINS
  (`snapshotBankValuesIntoModel()` L6965 + `chainModel_.toValueTree()`) ;
  restauration via `setStateInformation` → branche CHAINS L5708-5711 →
  `projectChainValuesToBanks()` L6989.
- **Presets** : `ui/ChainPresetIO.h` (`.sp3chain`, `chainsSchema=3`).
- **UI rack** : `ui/ChainRackComponent` — bandeau d'en-tête par chaîne
  (`Band`, `.h:230`, `kHeaderH=18`), peint en `.cpp:838-877`
  (« CHAIN n » à gauche, `×` à droite, espace libre entre les deux), menu
  contextuel d'en-tête `.cpp:962-1011` (Duplicate / Save preset / Load…).

## 3. Décisions de conception

### D1 — Encodage canonique unique
La propriété de chaîne adopte la convention C commune : **0 = Black,
1 = White, 2 = Auto**. Défaut : **White (1)** — le défaut actuel de 10 types
sur 11 (« paper is the typical Sp3ctra stream »). Les trois ordres de choix
APVTS disparaissent avec les paramètres ; plus aucune table de remap.

Conséquence assumée : MIDI TAP passait par défaut en Auto ; il suivra
désormais le défaut White de sa chaîne (les sessions existantes gardent leur
valeur via la migration).

### D2 — La propriété vit dans le MODÈLE, pas dans l'APVTS (V1)
`Chain::backgroundMode` (int), sérialisé comme attribut du nœud `CHAIN`.
Justification : c'est l'exact pattern du chantier J2/J3 (« la chaîne possède
les réglages ») ; pas de problème d'index instable (les chaînes n'ont pas de
slot stable, leur position bouge à la suppression) ; la persistance,
`duplicateChain` et les presets .sp3chain tombent juste naturellement.

**Perte assumée en V1** : le fond n'est plus automatisable hôte ni
MIDI-mappable (il l'était par module via MidiLearnAttachment). Si le besoin
live se confirme, V2 : projection APVTS `chain{i}_Background` indexée par
position, reprojetée à chaque édition structurelle (même mécanique que la
projection des enables). À ne PAS faire en V1 : deux sources de vérité.

### D3 — Propagation par le thread message uniquement (pas de changement RT)
`applyConfigurationToCore()` reste **l'unique écrivain** des configs modules.
Les blocs de synchro par type n'écrivent plus `background_mode` (les structs
`c` partent du défaut du module) ; **après** tous les blocs poolés, une passe
unique parcourt `chainModel_.chains` :

```
pour chaque chaîne ch (index c) :
    pour chaque module mi de ch.modules :
        slot = poolSlotForInstance(mi.id)   // poolés
               ou mi.slot                    // MidiTap
        lux_<type>_instance(slot)->config.background_mode = ch.backgroundMode;
```

Le parcours du modèle élimine le besoin d'un mapping inverse slot → chaîne
(qui n'existe pas). La même écriture est ajoutée au chemin rapide
`deriveChainRouting()` (comme `config.enabled` aujourd'hui) pour qu'un
déplacement de module entre chaînes applique le fond sans attendre une
édition de paramètre. Un changement du fond de chaîne déclenche
`applyConfigurationToCore` (ou au minimum cette passe) + `markStateDirty()`.

Aucun changement dans `chain_plan.h`, `image_chain.c`, `multithreading.c` :
les mécanismes de réaction au changement de mode déjà présents dans les
modules (reset LUT, ré-armement AUTO) suffisent. L'ajout d'un champ
`background_mode` à `SynthChainPlan` reste une option future si un
consommateur RT hors modules en a besoin (ex. fond du bus visuel).

### D4 — AUTO pour Pitch et Mask
Le sélecteur de chaîne propose {Auto, Black, White}. Pitch et Mask n'ont pas
de mode AUTO aujourd'hui. **Recommandation : leur ajouter le détecteur AUTO
standard** (pattern verrouillé-après-fenêtre, copié de lux_eq/lux_reverb,
~50 lignes par module ; « polarity is a property of the SOURCE »). Alternative
de repli si l'on veut réduire le périmètre V1 : pour Pitch/Mask, résoudre
Auto → White dans la passe de propagation (documenté dans le code).

### D5 — Purge des paramètres (pas de conservation cachée)
Les 88 paramètres sont **supprimés** (précédent : purge des knobs de
conditioning per-OUT du 2026-08-05, suppression de `chainInsertOrder`).
La compat sessions ne passe pas par les paramètres : elle passe par la
migration du modèle (D6). Les attributs `BackgroundMode` restés dans des
VALUES/MEMORY d'anciennes sessions sont simplement ignorés par
`projectChainValuesToBanks()` (qui itère le manifest, purgé).

### D6 — Migration des sessions et presets (schéma CHAINS 3 → 4)
`kSchemaVersion` passe à **4**. À la lecture d'un arbre `CHAINS` de schéma < 4
(dans `ChainModel::fromValueTree` ou juste après, côté processeur) :

1. Pour chaque `CHAIN` sans attribut `background` : parcourir ses `MODULE` en
   ordre, prendre la **première** valeur `BackgroundMode`/`backgroundMode`
   trouvée dans leur `VALUES`, la normaliser selon la famille
   (FX : {Auto,Black,White}={2,0,1} ; Pitch/Mask : {Black,White}={0,1} ;
   MidiTap : déjà {0,1,2}), et l'écrire comme fond de chaîne.
2. Aucun module porteur (ou session pré-v3 sans VALUES) → défaut White.

`ChainPresetIO` : écrire l'attribut sur le nœud `CHAIN` du preset ; à la
lecture d'un `.sp3chain` avec `chainsSchema` < 4, appliquer la même
dérivation. Les MIDI mappings pointant vers les ids supprimés doivent être
ignorés silencieusement au chargement (à vérifier dans MidiMap, point V-7).

## 4. Jalons

### B1 — Modèle + persistance + migration
- `ui/ChainModel.h` : `int backgroundMode {1};` dans `Chain` + identifiant
  `kBackgroundProp` (« background ») ; `kSchemaVersion = 4`.
- `ui/ChainModel.cpp` : `toValueTree()` écrit l'attribut ;
  `fromValueTree()` le lit + **migration D6** (dérivation depuis les VALUES
  des modules pour schéma < 4) ; `duplicateChain()` : vérifier que la copie
  emporte la propriété (membre simple — devrait être automatique) ;
  `makeDefault()` : chaînes à White.
- `ui/ChainPresetIO.h` : écriture + lecture + migration presets.
- Build vert ; à ce stade les modules suivent encore leurs propres params
  (état intermédiaire inoffensif).

### B2 — Propagation + purge des paramètres
- `PluginProcessor.cpp` :
  - passe de propagation D3 en fin de section poolée de
    `applyConfigurationToCore()` + écriture dans `deriveChainRouting()` ;
  - suppression des 11 créations de paramètres et des lignes
    `background_mode` + tables `k*BgChoiceToMode` des 11 blocs de synchro ;
  - un setter processeur `setChainBackground(int chainIdx, int mode)` :
    écrit le modèle, `persistChainModel()`, relance la propagation.
- `ui/ModuleParamManifest.h` : retirer les 11 suffixes.
- Vérifier compile : plus AUCUNE référence aux ids `*BackgroundMode`.

### B3 — UI
- `ui/ChainRackComponent` : sélecteur de fond dans le bandeau d'en-tête de
  chaque chaîne (espace libre entre « CHAIN n » et le `×`) : pastille d'état
  (□ White / ■ Black / A Auto) cliquable ouvrant un petit menu
  {Auto, Black, White} ; hit-test dans `mouseDown` du bandeau ; repaint sur
  édition. Réglage hors du cadre graphique des éditeurs, conformément aux
  règles UX du projet.
- Retirer combo + label + attachments Background des 10 TabComponents et de
  la ligne COMMON de `MidiTapPage.h` ; retasser les layouts.
- Vérifier qu'aucun éditeur ne lisait le paramètre APVTS pour son rendu
  (les rendus lisent l'état C du module, qui conserve le verdict résolu).

### B4 — AUTO pour Pitch/Mask (D4, périmètre ajustable)
- `processing/lux_pitch.{h,c}`, `processing/lux_mask.{h,c}` : ajout de
  `BG_AUTO` + détecteur standard (fenêtre + verrou, re-armé au reset).

### B5 — Vérification + documentation
- Passe `verify` complète (scénarios §5), mise à jour README/notes de
  chantier, mise à jour de la recette « nouveau module de chaîne » (un module
  ne déclare PLUS de BackgroundMode ; il hérite du fond de sa chaîne).

## 5. Plan de vérification (skill verify / build_vst.sh --run)

- **V-1 Propagation** : chaîne avec EQ + LEVELS + REVERB ; basculer le fond
  White → Black dans le rack → le rendu et le comportement audio de TOUS les
  modules basculent (plus de bandes grises, pôle de silence respecté).
- **V-2 Persistance** : sauver/recharger la session → le fond par chaîne
  revient (schéma 4).
- **V-3 Migration session** : charger une session schéma 3 dont les modules
  avaient un fond explicite → la chaîne hérite de la valeur (vérifier les
  trois familles d'encodage : FX, Pitch/Mask, MidiTap).
- **V-4 Presets** : round-trip `.sp3chain` v4 ; chargement d'un preset
  ancien → dérivation correcte.
- **V-5 Structure** : dupliquer une chaîne (fond copié) ; supprimer une
  chaîne (aucun décalage — propriété du modèle) ; déplacer un module vers
  une chaîne au fond différent → il adopte le fond de la chaîne d'arrivée
  (chemin rapide deriveChainRouting).
- **V-6 MIDI TAP** : capture black MIDI sur fond noir et blanc, pilotée par
  le fond de chaîne.
- **V-7 MIDI map** : session avec un ancien mapping sur `*BackgroundMode` →
  chargement silencieux, pas de crash.
- **V-8 AUTO** : chaîne en Auto sur flux papier puis sur flux inversé →
  verdicts corrects, y compris Pitch/Mask si B4 réalisé.

## 6. Risques et points de vigilance

- **Ordre d'écriture dans applyConfigurationToCore** : la passe D3 doit
  s'exécuter APRÈS les blocs par type (affectations `config = c` entières qui
  partent des défauts) — sinon le fond est écrasé à chaque synchro.
- **Index par famille** : poolés → `poolSlotForInstance(uuid)` ; MidiTap →
  `mi.slot`. Ne pas mélanger.
- **Trois encodages legacy** dans la migration (D6) — c'est LE piège du
  chantier ; tests V-3 sur les trois familles obligatoires.
- **MidiTap change de défaut effectif** (Auto → White du fond de chaîne) pour
  les nouvelles sessions — assumé (D1), à mentionner dans les notes de
  version.
- ~~Littéraux non-ASCII~~ : tout nouveau texte UI passe par `fromUTF8` si
  besoin (standard projet).
- Compat Windows CI : aucun code plateforme touché, mais passer la CI avant
  de pousser sur master (la release bêta se rafraîchit au push).

## 7. Hors périmètre / suites possibles

- Projection APVTS `chain{i}_Background` pour automation hôte + MIDI learn
  (V2, si besoin live confirmé).
- Champ `background_mode` dans `SynthChainPlan` pour des consommateurs RT de
  niveau chaîne (ex. couleur de fond du bus visuel / affichage rack).
- Verdict AUTO partagé au niveau chaîne (un seul détecteur en tête de chaîne
  au lieu de 11) — non retenu en V1 : les détecteurs par module s'adaptent au
  flux qu'ils voient réellement (important en aval d'un LEVELS en mode
  Invert).
