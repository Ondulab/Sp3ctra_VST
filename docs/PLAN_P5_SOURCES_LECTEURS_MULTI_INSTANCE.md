# PLAN — P5 : sources média et lecteurs score MULTI-INSTANCE (comme le Sampler)

> Établi le 2026-07-14 (post-P4), sur ordre user : « IMAGE/VIDEO/CAMERA et la
> famille score (SCORE/TIMBRE/MIDI SCORE/VOICE) doivent pouvoir être posés sur
> n'importe quelle chaîne en étant indépendants comme le sampler. »
> C'est l'écart (a) de la doctrine 2026-07-11 (« tout module est multi-chain,
> une instance indépendante par chain ») appliqué aux deux dernières familles
> singleton.
>
> ⚠️ COORDINATION : une seconde session Claude retravaille en ce moment la face
> SOURCES (SourcesTabComponent.h, VoiceGenTabComponent.h, non commité). Les
> jalons M2+ touchent ces fichiers — démarrer M2+ APRÈS le commit de ce
> chantier. M1 (modèle/plan) est sans collision.

## État de départ (vérifié, post-P4 commit 7831add)

### Sources média (IMAGE / VIDEO / CAMERA)
- Pool C `internal_source` keyé par KIND (INTERNAL_SRC_IMAGE/VIDEO/CAMERA) :
  UNE ligne publiée par type. `internal_source_copy(kind, …)` ; le plan porte
  `source_kind` (CHAIN_SRC_IMAGE/…), `synth_source_base` copie par kind dans
  le scratch par chaîne — l'INSTANCE n'existe pas côté RT.
- Moteurs singletons pilotés par MediaSourceService (ImageSource/VideoSource/
  CameraSource engines, AVFoundation/juce_video pour VIDEO/CAMERA).
- Params GLOBAUX hors manifest (imgSrc*/vidSrc*/camSrc*, chemins média,
  transport par module imgSrcPlay/vidSrcPlay) — exclusion documentée J1.
- ChainModel : 1 instance par type (singleton canInsertIntoNewChain).
- Sweep silence/contrat blanc OK (source-active-white-contract).

### Famille score (SCORE / TIMBRE / MIDI SCORE / VOICE)
- UN buffer partagé : `scoreSlot` du moteur A (LuxSampler), écrit par
  loadScoreFramesFromImage — les 4 générateurs écrasent le même contenu.
- UN transport partagé (uiPlayScore/uiStopScore, isScorePlaying channel-wide).
- Plan : marqueur IMAGE_CHAIN_INSERT_SCORE avec state_idx=0 (inutilisé) +
  flag has_score par chaîne ; `chain_player_owned(is_score=1)` matche TOUTES
  les chaînes has_score → une lecture joue dans toutes les chaînes score.
- Lecture : runScoreSession sur le FramePlayerThread du moteur A (1 kHz),
  relay/éviction avec le sampler (scoreRelaySlot_, s_enginesShareChain).
- kScoreFamily/isScoreFamily (ModuleCatalog.h) = LA liste, prête à servir.

### Acquis P4 réutilisables (le gros du chemin est fait)
- `chain_execute_span` unique + `chain_player_execute_owned` : la lecture d'un
  player marche déjà PAR CHAÎNE via son marqueur — il suffit que le marqueur
  porte l'IDENTITÉ D'INSTANCE (comme SAMPLER : insert_state_idx = slot moteur).
- `chain_owning_marker_pos(sp, score_owns, engine_slot)` : le split-point sait
  déjà matcher un slot précis.
- `chain_send_transport` : l'autorité par chaîne absorbera les transports par
  instance sans nouveau mécanisme.
- Patterns éprouvés : pools par UUID→slot (modulePoolSlots_), banques ×8 du
  manifest (J1), rebind UI par instance (sampler A/B), reset différé des slots
  orphelins, presets .sp3chain.

## Architecture cible

### A — Sources média poolées
- `internal_source` : (kind, slot) — `INTERNAL_SRC_SLOTS = 8` par kind (tableau
  ligne+flags par slot). API : `internal_source_copy_slot(kind, slot, …)`,
  publish/clear par slot ; le sweep blanc reste par slot.
- ChainPlan : `SynthChainPlan.source_slot` (0..7) rempli depuis
  ModuleInstance.slot du module source ; `synth_source_base` copie
  (kind, source_slot).
- Moteurs ×N : MediaSourceService gère un registre d'engines par (kind, slot),
  création/destruction au fil du modèle (pattern registre LuxSampler).
  CAMERA : device sélectionnable PAR INSTANCE (deviceId dans la banque) — deux
  instances peuvent partager le même device physique (readers multiples) ou
  pointer deux caméras.
- Params : entrée manifest pour les 3 types (imgSrc{N}_*, vidSrc{N}_*,
  camSrc{N}_* ×8 — enabled/play/vitesse/chemin…), VALUES/typeMemory/presets
  gratuits via J2/J3. Chemins média persistés par instance.
- UI : SourceSetupPanel/tabs rebindés à l'instance sélectionnée (pattern
  sampler) ; LED rack par instance.

### B — Lecteurs score poolés
- `ScoreSlotPool` : `kMaxScorePlayers = 8` slots {frames, meta, transport,
  play-head} — sortis de LuxSampler (le scoreSlot du moteur A disparaît).
  Chaque instance score-family (UUID→slot, pool PARTAGÉ par la famille — un
  TIMBRE et un SCORE ne se marchent plus dessus) possède SON buffer : les
  générateurs écrivent LEUR slot.
- Plan : marqueur SCORE avec `insert_state_idx = slot d'instance` ;
  `has_score` reste le flag « la chaîne héberge un score-family » mais
  l'ownership devient PAR SLOT : `chain_player_owned(is_score, slot)` matche
  le marqueur exact (symétrie totale avec SAMPLER/engine).
- Lecture ×N : un `ScorePlayerService` possède N sessions 1 kHz (thread unique
  multiplexant les slots actifs, tick par slot — pas N threads) ; chaque tick
  produit la frame du slot puis appelle `chain_player_execute_owned(is_score=1,
  slot, …)` → la marche P4 fait le reste (OUTs, probes, record downstream,
  taps) UNIQUEMENT dans les chaînes portant CE marqueur.
- Transports par instance : play/stop/scrub par slot (LED par module au rack) ;
  `chain_send_transport` : la branche sampler/score lit le transport DU slot
  du marqueur de la chaîne.
- Relay sampler : l'éviction ne s'applique plus que si le score-slot et le
  sampler partagent LA MÊME chaîne (généralisation de s_enginesShareChain).
- `lux_sampler_is_score_playing()` (global) → `score_slot_is_playing(slot)` ;
  les gates udp/feeder (`stream_player_owned`, `chain_owning_marker_pos`)
  matchent le slot driving comme pour les moteurs sampler.

## Jalons

### P5-M1 — Modèle + plan : instancier sans changer le runtime (M) — ✅ FAIT (2026-07-14)

Statut : implémenté, build vert. Pools `firstFreeMediaSlot(type)` (8/kind) +
`firstFreeScorePlayerSlot()` (8 partagés par la FAMILLE via isScoreFamily) ;
singletons levés (seul SEQUENCER reste singleton) ; budgets + healing
validateAndRepair ; hasSlot étendu (persistence kSlotProp + pré-seed presets
gratuits) ; plan : `SynthChainPlan.source_slot` + marqueur SCORE avec
state_idx = slot d'instance (consommateurs matchent encore par id → parité).
Vieilles sessions : slot -1 → heal slot 0. LIMITE M1 documentée : N instances
posables mais runtime kind-wide (mêmes contenus) jusqu'à M2/M4.

#### Plan initial (référence)
- ChainModel : lever les singletons IMAGE/VIDEO/CAMERA/SCORE/TIMBRE/MIDI
  SCORE/VOICE → types poolés (slots 0..7 par famille : médias par kind, score
  par FAMILLE via kScoreFamily) ; validateAndRepair budgets + healing ;
  1/type/chaîne conservé (D5).
- deriveAndPublishChainPlan : `source_slot` + marqueur SCORE avec state_idx =
  slot d'instance. RUNTIME INCHANGÉ : internal_source_copy ignore encore le
  slot (lit le kind), le lecteur score reste unique — instance 0 = comportement
  actuel, les instances >0 existent au rack mais silencieuses (documenté).
- Persistence : slots dans POOL_SLOTS (mécanique existante), schéma inchangé.

### P5-M2 — Pool internal_source par slot + moteurs média ×N (L)
Pool (kind, slot) + API slot ; registre d'engines MediaSourceService ;
feeder/udp lisent (kind, source_slot) ; sweep blanc par slot ; reset différé
des slots orphelins. IMAGE d'abord (trivial : N buffers), puis VIDEO, puis
CAMERA (devices par instance).

### P5-M3 — Params média au manifest + UI par instance (M)
Banques ×8 des 3 types au ModuleParamManifest (VALUES/typeMemory/presets
gratuits) ; migration des params globaux imgSrc*/vidSrc*/camSrc* → banque
slot 0 ; SourceSetupPanel/tabs rebind par instance ; LED par instance.
⚠️ Après le chantier SOURCES de l'autre session.

### P5-M4 — ScoreSlotPool + lecture par slot (L — cœur de B)
Pool de slots score (frames sortis de LuxSampler) ; ScorePlayerService
(1 thread, N sessions 1 kHz) ; chain_player_owned/owning_marker par slot ;
relay sampler scoped par chaîne ; transports par slot ;
chain_send_transport branché sur le slot du marqueur. Le scoreSlot moteur A
et uiPlayScore/uiStopScore globaux meurent.

### P5-M5 — Générateurs + UI par instance (M)
Les 4 onglets (Score/Timbre/MidiScore/Voice) écrivent le slot de LEUR
instance (loadScoreFramesFromImage → slot) ; pages rebindées à l'instance
sélectionnée ; LED/transport par module au rack ; export/cache WAV (VOICE)
par instance.

### P5-M6 — Purge + matrice (S)
Purge du canal partagé résiduel (notifyScoreStopped global, isScorePlaying
channel-wide, scoreRelaySlot_ encodage moteur), commentaires ; matrice :
2 scores sur 2 chaînes en simultané, score+sampler même chaîne (le trou
d'arbitrage M2 se résout naturellement par slot), IMAGE ×2 chaînes avec
contenus différents, VIDEO+CAMERA simultanés, presets/reload, 0 underruns.

## Décisions à valider

| # | Question | Recommandation |
|---|---|---|
| D1 | Caps d'instances | 8 par famille (aligné pools existants) ; CAMERA limité par le hardware, pas par le modèle |
| D2 | Deux CAMERA sur le même device physique | Autorisé (readers partagés) ; device sélectionnable par instance |
| D3 | Lecture simultanée de N scores | OUI (c'est le but) — 1 thread multiplexé, pas N threads |
| D4 | Pool score par FAMILLE (SCORE+TIMBRE+MIDI SCORE+VOICE partagent les 8 slots) ou par type (8×4) | Par FAMILLE (8 lecteurs suffisent, budgets simples) |
| D5 | Sessions existantes | Instance unique → slot 0, parité totale au chargement |

> Ordre : M1 tout de suite (sans collision) ; M2/M3 après le commit du
> chantier SOURCES parallèle ; M4/M5 ensuite (B dépend peu de A). Chaque
> jalon : build vert + smoke + commit isolé.
