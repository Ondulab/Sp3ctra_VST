# PLAN — P3 (mix multi-chains + purge moteur B) puis « chain porteuse des réglages »

> Établi le 2026-07-11, branche `feat/synth-split-p1-out-banks`.
> Doctrine de référence : les chains manipulent exclusivement un flux vidéo (lignes d'images) ;
> **3 synthèses** (LuxStral, LuxSynth, LuxWave) ne reçoivent des données que via des modules OUT
> présents dans les chains ; tout module est multi-chain avec instance indépendante par chain ;
> la chain porte tous les réglages de ses modules (presets de chain à terme).

## État de départ (vérifié)

- **P3-M1 est livré** (`edb0f4c` + `0c7f2dd`) : staging par send LuxStral
  (`processing/synth_staging.c`, seqlock par slot de chain), mix pull sur le thread audio
  (`multithreading.c` ~2669-2738), `LsSendPlan`/`ls_send[]` dans le plan, pool LuxStral à
  **8 sends** (`ChainModel.h:93` `kMaxLuxStralEngines = kMaxChains`).
- Conséquence : **les chemins du moteur B sont déjà du code mort à l'exécution**
  (`num_ls_sends ≥ 1` dès qu'un LuxStral est posé → la branche mixer est toujours prise ;
  `synth_AudioProcess_ab`, blocs B udpThread/feeder, `luxstral_b_feed_player_frame` inatteignables).
- **Le vrai feed audio de LuxSynth passe par le thread UI** :
  `CisVisualizerComponent::computeFftMagnitudes()` (~30 fps) lit le bus `ENGINE_TAP_PATHB`,
  reconditionne `luxsynth_out[0]` et pousse via `luxsynth_engine_set_spectral_data`. La FFT
  « pipeline » (`preprocess_luxsynth`) n'alimente que les visualisations. Le mix LuxSynth doit
  couvrir les deux consommateurs (cf. décision D2).

---

# PARTIE A — P3 (suite) : jalons M2 → M8

## M2 — Retrait intégral du moteur LuxStral B — ✅ FAIT (2026-07-11, non commité)

Fait en premier : les chemins sont morts, la purge réduit la surface de tout le reste.
Statut : purge complète (~30 fichiers), build vert, smoke test standalone OK (session
2 sends ex-A/B rechargée : badge « 2 SEND », bancs 0/1 préservés, 0 underruns).
Notes d'implémentation : ids d'enveloppe renumérotés (LUXWAVE=3, LS_SEND_BASE=4,
usage interne image_pipeline.c) ; slot de plan 3 laissé en trou jusqu'à M3
(CHAIN_SYNTH_COUNT inchangé = maths de scratch intactes) ; la migration
synthSplitVersion lit le XML brut et survit à la suppression des params luxstralB* ;
LED rack LuxStral = lsOutParam(slot,"enabled") avec fallback deviceEnabled ;
luxStralEngineIndex_ devient le slot de send sélectionné (0..7, clamp élargi).
Reste à valider en conditions réelles : parité bit-exact mono-send, live UDP device,
score relay (couvert par la matrice M7).

Liste de purge :
- `multithreading.c` : `s_luxstral_b_db` + `luxstral_b_db_ensure_ready` + `luxstral_b_commit_silence` ;
  `luxstral_b_feed_player_frame`, `luxstral_b_player_stopped`, `luxstral_b_copy_preprocessed_gray` ;
  bloc B udpThread (~1720-1823) ; bloc B feeder (~2388-2426) ; branche `bActive`/`synth_AudioProcess_ab`
  de audioProcessingThread (~2742-2757). Exports `multithreading.h:185-218`.
- Synthèse : `g_luxstral_engine_b`, `synth_AudioProcess_ab/_b`, `synth_luxstral_init_engine_b`,
  `synth_luxstral_engine_b_ready`, buffers `luxstral_b_buffers_L/R` (+ consommation
  `PluginProcessor.cpp:2794-2842`), adapters B de `vst_adapters.cpp`.
- Pipeline : `pipeline_build_config_luxstral_b`, `ENVELOPE_LUXSTRAL_B`.
- Params/état : params + listeners `luxstralB*`, miroir B d'`applyConfigurationToCore` (~5108-5135),
  champs `luxstral_b_*` de `config_loader.h:231-248`, `luxstralBPresent_`.
  **Compat sessions** : la migration `synthSplitVersion` lit les valeurs `luxstralB*` dans le blob
  XML brut (pas l'APVTS) → elle survit à la suppression des params ; la conserver telle quelle.
  Topologies à 2 LuxStral = déjà 2 sends parmi 8 depuis M1 — pas de migration de modèle.
- UI : LED rack LuxStral → `lsOutParam(slot, "enabled")` ; vues `SPCTR_B_*` + `isB`
  (`PluginEditor.cpp:713-739`, `CisVisualizerComponent`, `VisualizerMode.h`), champs B de
  `LuxStralSetupPanel`/`LuxStralTabComponent`.
- ChainPlan : cesser de remplir `CHAIN_SYNTH_LUXSTRAL_B` ; garder le `#define` jusqu'à M3.

Validation : build vert ; topologie par défaut bit-exact (1 send, intensity 1) ; ancienne session
avec B → recharge en 2 sends, banks 0/1 peuplées ; 0 underruns ; scénarios live UDP, sampler,
score, chain sans source (silence vrai). Perte assumée : lanes d'automation hôte `luxstralB*`.

## M3 — ChainPlan uniforme : 8 recettes de chain, OUT = insert positionnel — ✅ FAIT (2026-07-11, non commité)

Statut : implémenté, build vert, smoke test OK (2 sends actifs via la boucle uniforme,
0 underruns). Réalisation effective :
- `plan.chain[]` (recettes complètes par chain, `num_chains`) REMPLACE `probe_chain[]` ;
  `synth[]` et `ls_send[]` restent remplis (viz/gating/mixeur/player). Marqueurs
  `IMAGE_CHAIN_INSERT_OUT_LUXSTRAL/LUXSYNTH/LUXWAVE` (pass-through), state_idx = banc.
- Exécuteur positionnel partagé `chain_execute_positional` (marche insert par insert :
  probes, tap sélection, staging LuxStral et capture Path-B à leur position exacte) +
  `chain_shortcut_walk` (chaînes sampler/score-relay : base/mod par position) — remplace
  les 4 blocs (probe loop, send loop, legacy A, Path-B) dans udpThread ET le feeder ;
  `feeder_sampler_chain_shortcut` supprimé ; scratch source réduit à 12 slots (4+chain_idx).
- Path-B nourri au marqueur OUT_LUXSYNTH (fallback OUT_LUXWAVE), une seule FFT en aval.
- Reset différé des stagings orphelins (`pendingStagingResets_`, pattern pool-reset 40 ms).
- Améliorations de comportement (au-delà de la parité) : probes APRÈS un OUT désormais
  capturés ; les FX ne tickent plus 2× sur une chaîne multi-OUT ; tap sélection sous un
  OUT LuxStral fonctionne ; sans module OUT LuxSynth/LuxWave, plus AUCUN feed Path-B
  (doctrine no-OUT = no-feed). Deltas assumés : chaîne sampler+OUT Path-B en idle — les
  FX entre marqueur et OUT ne tickent plus côté udp (sémantique relay).
Reste à valider en réel : matrice sampler/score/live UDP (couverte par M7).

- `image_chain.h` : marqueurs `IMAGE_CHAIN_INSERT_OUT_LUXSTRAL/LUXSYNTH/LUXWAVE` (pass-through
  dans `image_chain_run` ; `insert_state_idx` = slot de banc de l'instance OUT).
- `chain_plan.h` v2 : `ChainPlan { int num_chains; ChainRecipe chain[CHAIN_MAX_CHAINS]; }` —
  une recette complète par chain (source → inserts ordonnés, OUT et probes = inserts positionnels) ;
  `CHAIN_PLAN_MAX_INSERTS` porté à ~20 ; seqlock writer unique inchangé.
- `deriveAndPublishChainPlan` : une seule boucle chain (remplace le triple mécanisme
  fill()/ls_send/probe) ; une chain sans OUT/probe/sélection reste `present=0`.
- `multithreading.c` : exécuteur unique `chain_execute_recipe(...)` partagé udpThread/feeder —
  résolution source (`synth_source_base`, scratch réduit à `CHAIN_MAX_CHAINS` entrées), marche des
  inserts UNE fois par frame ; marqueur OUT_LUXSTRAL → conditioning per-send + staging ;
  OUT_LUXSYNTH/LUXWAVE → comportement actuel conservé (parité) jusqu'à M4/M5.
  `ls_sends_stage_player_frame` itère les recettes `has_sampler/has_score`.
- Reset différé (≥40 ms, pattern `pendingPitchResets_`) des stagings orphelins après republication
  du plan (delete/reorder de chain → réindexation).

Validation : parité bit-exact vs M2 (mono-send live, 2 sends LuxStral, chain probe-only,
sampler idle/PLAY, score relay, no-signal). CPU udpThread : une chain à 3 OUT ne parcourt plus
ses inserts 3×.

## M4 — Staging + mix pull LuxSynth (Σ lignes conditionnées, puis UNE FFT) — ✅ FAIT (2026-07-11, non commité, D2 inclus)

Statut : implémenté, build vert, vérifié live (send → LUXSYNTH posé au rack : badge
« 1 SEND », spectre FFT 128 harmoniques vivant + harmonicité, 0 underruns).
Réalisation effective :
- `luxsynth_condition_line()` extrait de preprocess_luxsynth (étapes 1-4, sans
  intensity) — partagé pipeline/producteurs.
- `synth_staging_stage_luxsynth/mix_luxsynth` (ligne conditionnée + RGB brut par chain,
  seqlock ; mix gaté par le PLAN — un staging orphelin ne fuit jamais ; générateur de
  dirty-check).
- **D2 réalisé** : nouveau module `processing/luxsynth_feed.c` — le feed spectral du
  moteur vit sur audioProcessingThread (mix → Hann → kiss_fftr → normalisation pic →
  smoothing dt-corrigé (lx_fft_* mirrorés dans g_sp3ctra_config + listeners) →
  harmonicité couleur → set_spectral_data). Le bridge UI est retiré : **LuxSynth sonne
  éditeur fermé** ; la FFT de CisVisualizer est désormais display-only (la sync config
  moteur y reste).
- Producteurs : marqueur OUT_LUXSYNTH stagé par l'exécuteur positionnel + la marche
  shortcut (udp/feeder) ; single-writer pendant playback via
  `lx_send_stage_player_frame` (FramePlayerThread, au site du tap PATHB).
- Gates : transport Chain-2 (PLAY/HOLD/STOP), 0 send → silence poussé une fois ;
  reset différé étendu aux sends LuxSynth.
Simplifications V1 documentées : enveloppe par send (ENVELOPE_LX_SEND) différée à M6
(singleton → le gate global équivaut) ; la FFT display de l'UI lit encore TAP_PATHB
(l'accesseur copy_mix viendra avec le multi-send M6).

- Scinder `preprocess_luxsynth` : `luxsynth_condition_line(raw, bank, line_out)` (étapes 1-4,
  sans intensity) + `preprocess_luxsynth_from_line(line, r,g,b, out)` (FFT + color FFT).
- `synth_staging_stage_luxsynth(chain_idx, bank, line, r,g,b, nb_pixels)` — staging par chain
  avec ligne float + RGB (nécessaire à la color-FFT) ; producteur = exécuteur M3 au marqueur
  OUT_LUXSYNTH, après envelope per-send (gating `image_freeze_mode` conservé).
- Mix pull sur audioProcessingThread (même site que le mixer LuxStral) :
  `line = clamp01(Σ w_k·line_k)`, RGB = moyenne pondérée ; puis UNE FFT →
  commit `preprocessed_data.polyphonic` ; aucun send actif → zérotage (contrat no-signal).
  FFT relancée seulement si un `seq` de staging a changé (dirty check).
- Feed UI : `computeFftMagnitudes` cesse de lire `TAP_PATHB` + reconditionner slot 0 →
  nouvel accesseur `synth_staging_copy_luxsynth_mix(...)` (snapshot du mix, double-buffer).
- Player : stager les OUT LuxSynth des chains sampler/score dans
  `ls_sends_stage_player_frame` généralisé ; les commits polyphonic gardés par
  `luxsynth_source_type==0` disparaissent (retrait complet du garde en M7).

Validation : mono-send intensity 1 → spectre/timbre identiques ; 2 chains → 1 LuxSynth
(poids intensity respectés) ; `lxFftSmoothing` inchangé ; freeze/HOLD conservé ;
0 underruns avec la FFT sur l'audio thread (mesure rt_profiler ; repli : thread dédié non-RT).

## M5 — Staging + mix pull LuxWave — ✅ FAIT (2026-07-11, commité avec M5)

Statut : implémenté, build vert, smoke test OK (0 underruns ; chemin no-send validé —
le test d'écoute avec un module → LUXWAVE + MIDI reste à faire, matrice M7).
Réalisation effective : `luxwave_condition_line()` (banc, sans intensity) +
`synth_staging_stage_luxwave/mix_luxwave` (mix bipolaire 0.5 + Σ w·(line−0.5), gaté
par le plan, D3) ; `pipeline_luxwave_feed_tick()` sur audioProcessingThread (mix →
ENVELOPE_LUXWAVE avec gates live/raw — fade timestamp-driven donc insensible à la
cadence — → `luxwave_engine_set_image_line`, ~86 Hz vs line-rate avant : moins de
pushes qu'avant, pas plus) ; feed inline retiré de `pipeline_path_luxsynth_luxwave`
(qui ne garde que l'enveloppe Chain-2 des vues polyphoniques + copy photowave) ;
producteurs aux marqueurs OUT_LUXWAVE (positionnel + shortcut) ; resets/no-signal
étendus. Divergence assumée : l'enveloppe s'applique après le mix (identique à 1 send
intensity 1 ; transitoire de fade légèrement différent à k≠1). LuxWave n'est jamais
player-fed (parité : c'était déjà le cas).

- Producteur au marqueur OUT_LUXWAVE : conditioning `luxwave_out[bank]` + envelope per-send →
  `synth_staging_stage_luxwave(chain_idx, bank, line)`.
- Mix bipolaire autour de 0.5 : `mixed[i] = clamp01(0.5 + Σ w_k·(line_k[i] − 0.5))`
  (seule loi qui préserve la sémantique intensity actuelle et la parité mono-send — cf. D3).
- Consommateur : même site que M4 ; `luxwave_engine_set_image_line` déjà thread-safe ;
  push seulement sur staging changé (préserve le crossfade wavetable) ; aucun send actif →
  ne rien pousser (le moteur garde sa dernière table, ne sonne que sous MIDI — comportement actuel).
- `pipeline_path_luxsynth_luxwave` se vide (copy photowave résiduel ou suppression).

Validation : mono-send parité ; 2 sends → table = mix bipolaire ; intensity 0 → table plate 0.5.

## M6 — OUT poolés par instance + levée des contraintes d'insertion — ✅ FAIT (2026-07-11)

Statut : implémenté, build vert, smoke test OK. Réalisation effective :
`ChainModel::isEngineSend` + `firstFreeEngineSendSlot(type)` (pool 8 slots PAR TYPE,
indépendants) ; LuxSynth/LuxWave retirés de la règle singleton (multi-chain N≤8,
1/type/chaîne conservé — D5) ; `validateAndRepair` budgets + healing par type ;
marqueurs OUT lx/lw portent le slot d'instance réel ; pb_chain = premier OUT_LUXSYNTH
en ordre modèle (vues polyphoniques mono-chaîne assumées) ; éditeur : `setTarget(type,
slot d'instance)` + power zone-3 par banc pour les 3 types ; rack : LED +
setEnableParamOverride par banc pour les 3 types. Anciennes sessions : slot=-1 →
heal vers slot 0 (banc legacy, parité). Vérif interactive multi-send (2-3 OUT
LuxSynth, pages indépendantes, badges) à faire à la main — je ne pilote plus l'UI.

- `ChainModel` : pool généralisé `isEngineSend(t)` (LuxStral/LuxSynth/LuxWave,
  `kMaxEngineSends = kMaxChains`) ; retirer LuxSynth/LuxWave de la règle singleton de
  `canInsertIntoNewChain` (la règle « pas de doublon de type par chain » les couvre — V1 : 1 OUT
  par type et par chain, cf. D5) ; `validateAndRepair` : budgets + healing de slots pour les
  3 pools ; sessions anciennes slot=-1 → heal vers slot 0 (parité). Pas de bump `kSchemaVersion`.
- `PluginEditor` : `luxStralEngineIndex_` → `sendSlot` générique 0..7 résolu de l'instance
  sélectionnée ; `setTarget(type, slot)` pour les 3 types ; power zone-3 par slot.
- `ChainRackComponent` : LED des 3 OUT = `*OutParam(slot, "enabled")`.
- `AudioMixPanel` : compte des sends déjà générique ; strips gardent l'enable moteur global.

Validation : 2-3 OUT LuxSynth sur chains distinctes, chaque page OUT édite SON banc
(pas de « linked params » — piège attachments : reset(nullptr) avant recréation) ;
badges « n SEND » ; save/reload conserve slots et bancs ; 9e send refusé proprement.

## M7 — Purge de la double autorité `*_source_type` + tags `dataReady`

- Le plan (recettes + stagings) devient l'unique autorité de routage : suppression des gardes
  `luxstral_source_type`/`luxsynth_source_type` sur les commits (udpThread ~1910-1955,
  feeder ~2478-2504, `LuxSampler.cpp` ~3336-3351 et ~3457-3515), du SRC-GATE et du fallback live
  du consommateur (`synth_luxstral.c:796-885` — no-signal ⇒ sections zérotées, jamais de fallback),
  du tag `dataReady` 1/2.
- Supprimer `setChainSourceRouting`/`chainSrcLuxstral/Luxsynth`, `sourceChannelForSynth`
  (si plus consommé), champs legacy de `config_loader.h:221-229` ; les lectures display
  (`VideoDisplayComponent`, `VideoScrollTab`) basculent sur les buses engine-tap.
- Contrat no-signal préservé : `synth_source_base == -1` → `synth_staging_set_inactive` +
  zérotage mixer.

Validation : matrice complète — live UDP ; feeder IMAGE/VIDEO/CAMERA ; sampler
idle/REC/PLAY/STEP_EMPTY/STEP_LIVE ; score relay ; chain sans source → silence vrai ;
hot-swap device↔feeder.

## M8 — Nettoyage final

`pipeline_build_config_live/sampler` purgés des champs lus depuis `luxstral_out[0]`/
`luxsynth_out[0]` au profit du per-send ; `ENVELOPE_CHAIN2`/`ENVELOPE_LUXWAVE` si remplacés ;
`#define CHAIN_SYNTH_*` restants ; commentaires/headers (dont `PluginProcessor.cpp:2902`
« LuxWave reads the LuxSynth grayscale line », obsolète).

## Risques P3

1. **Pacing LuxStral** : mix pull déjà cadencé par le handshake consommateur — inchangé ;
   la purge B supprime le pacing B-sur-A.
2. **FFT LuxSynth sur l'audio thread** : borné par dirty-check + mesure rt_profiler ;
   repli = thread dédié non-RT.
3. **Réindexation des chains** : stagings keyés par index modèle → reset différé des slots
   orphelins ; le banc lu depuis le snapshot du staging borne le mal à une frame.
4. **Chain muette** : no-signal explicite ⇒ contribution nulle ; producteur simplement silencieux
   ⇒ dernière valeur tenue (parité held-frame) — cf. D4.
5. **Resize/reset** : stagings statiques (`PREPROCESS_MAX_NOTES`/`CIS_MAX_PIXELS_NB`),
   re-init sur changement DPI, `num_notes/nb_pixels` portés par snapshot.

---

# PARTIE B — La chain porteuse des réglages (post-P3)

Principe directeur : le tree `<CHAINS>` (v3) devient la **source de vérité au repos**
(persistée, preset-able) ; les banques APVTS ×8 restent le **registre d'exécution**
(live, automatable — les params APVTS étant statiques, on ne peut pas en créer par chain).
Projection chain→banque au chargement/à l'édition ; snapshot banque→chain à la sauvegarde/au
retrait. **Pas de write-through continu** knob→tree (inutile + source classique de boucles
`parameterChanged`→`replaceState`). Pas de listener ValueTree sur CHAINS → aucune boucle possible.

## J1 — Manifest unique des params par module (refactor iso-comportement)

Nouveau `ui/ModuleParamManifest.h/.cpp` : table unique `ModuleType` → { builder d'id
`(slot, suffix)`, suffixes, numSlots, foncteur de résolution de slot (pooled UUID→slot vs
`m.slot`) }. Y migrent : `kInsertBanks` (Pitch/Mask/Reverb/Echo/EQ ×8), VideoScroll ×8
(+ `videoMix{N}_*`), les 3 bancs OUT ×8, Sampler ×2 (`fsEngineParam`). Hors manifest
(documenté) : params moteur globaux, sources (`imgSrc*`/`vidSrc*`/`camSrc*`/`acqGate*`),
SCORE/SEQ (non-APVTS). Listeners + migration legacy itèrent le manifest.
Validation : dump des paramIDs identique avant/après ; sessions existantes sans diff.

## J2 — CHAINS v3 : valeurs embarquées

- `ModuleInstance` gagne un `ValueTree values` (`<VALUES suffix="raw">`) ; `kSchemaVersion = 3`.
- `snapshotBankValuesIntoModel()` : lit `getRawParameterValue` (atomique) par module/suffixe —
  appelé dans `getStateInformation` avant la sérialisation de CHAINS (copie fraîche du tree).
- `projectChainValuesToBanks()` : `setValueNotifyingHost` **seulement si différent**
  (sinon toutes les lanes d'automation sont marquées « touchées » à chaque open) — appelé après
  `loadChainModelFromState()` pour un blob v3, et au load de preset. Message thread exclusivement.
- Une session v2 charge sans projection (banques plates autoritaires) et se réécrit en v3 au save.

Règle définitive : knob/automation → banque uniquement ; banque → tree aux snapshots
(save, retrait/déplacement) ; tree → banque aux projections (load, ajout héritant, preset).

## J3 — Cycle de vie : la chain mémorise, INSERT_MEMORY subsumé

- Mémoire par chain, par type, **dans le tree de la chain** (`<TYPE_MEMORY>`), étendue à TOUS les
  types du manifest (aujourd'hui limitée aux 5 pooled inserts — VideoScroll et OUT n'ont aucune
  mémoire de chain).
- Ajout : reset défauts → projection du TYPE_MEMORY de la chain hôte (héritage) → Enabled ON.
- Retrait : snapshot banque → TYPE_MEMORY de la chain (les valeurs « restent dans la chain »).
- Déplacement inter-chain : **les réglages suivent l'instance** (slot stable par UUID, zéro
  écriture) ET la chain source garde son snapshot — la doctrine est satisfaite au repos (cf. A).
- Suppression de chain : la mémoire meurt avec le tree. Duplication de chain (nouvelle op) :
  copie du tree avec VALUES, UUIDs neufs, insertion validée module par module, projection →
  deux instances indépendantes aux mêmes réglages. Undo : hors-scope.
- Migration : au load d'un blob portant INSERT_MEMORY, injection one-shot dans les TYPE_MEMORY ;
  on cesse d'écrire INSERT_MEMORY. **POOL_SLOTS reste tel quel** (garantit la stabilité
  slot↔instance dont dépendent lanes d'automation et mappings MIDI).

Validation : retrait/ré-ajout d'un Pitch réglé → restauré (parité) ; idem VideoScroll et OUT
(nouveau) ; 2 Pitch sur 2 chains indépendants ; déplacement inter-chain conserve les réglages.

## J4 — Presets de chain `.sp3chain`

- Format XML ValueTree lisible : `<SP3CHAIN version name synthSplitVersion chainsSchema>` +
  `<CHAIN>` avec `<MODULE type><VALUES/></MODULE>` — sans UUIDs (régénérés), sans slots
  (réassignés). V1 = topologie + VALUES du manifest. Exclus V1 (documenté) : slots audio Sampler
  (.sp3s), image/WAV Score, chemins media, pattern Sequencer, mappings MIDI. V2 : `<SCORE>` +
  chemins media optionnels.
- Load : deux modes — *Load into this chain* / *Load as new chain* (refusé si `!canAddChain()`).
  Validation module par module (`canInsert`, en excluant les modules remplacés) ; type
  indisponible (singleton placé ailleurs, pool épuisé) → **sauté avec dialogue récapitulatif**
  (`Sp3ctraDialog`), jamais d'échec atomique.
- Flux load (message thread) : muter le modèle → pré-seeder `modulePoolSlots_` (stabilité, J5) →
  `onChainModelEdited()` → projection → rebuild UI (pattern `onStateRestoredUi` ; piège
  attachments : reset(nullptr) avant recréation).
- UI : clic droit sur le header de chain dans `ChainRackComponent` → PopupMenu
  { Save chain preset…, Load into this chain…, Load as new chain…, Duplicate chain } ;
  FileChooser async + écriture atomique `juce::TemporaryFile`.
- Nouveau `ui/ChainPresetIO.h/.cpp` + API processeur `saveChainPreset`/`loadChainPreset`.

Validation : save → New session → load → XML identique ; pool plein → dialogue + chain partielle ;
load pendant lecture → 0 xrun.

## J5 — Stabilité automation DAW / MIDI

Les params automatables restent les banques par slot ; recharger un preset peut réassigner N.
Politique : *Load into this chain* pré-seed `modulePoolSlots_[nouveauUuid] = ancienSlot` pour les
types partagés avec l'ancienne composition → remplacer un Pitch par un Pitch garde le slot
(automation + MIDI survivent) ; types nouveaux → lowest-free-slot ; pas de re-learn auto V1
(badge « mapping orphelin » optionnel via `navTargetForParam`) ; limite documentée dans le
dialogue de load. Validation : CC mappé sur `luxpitch0_*` survit à un save/load de preset.

## J6 — Purge et durcissement

Suppression `insertParamMemory_`/`prevInsertLoc_`/`baselineInsertLocations` (lecture-migration
conservée 2 versions) ; log debug de la taille du blob ; test de non-régression migration
(blob synthSplitVersion=0/1 + INSERT_MEMORY + POOL_SLOTS → charge, se resauve en v3 propre).

## Risques B

- `getStateInformation` hors message thread (certains hôtes) : le snapshot lit des atomiques mais
  itère `chainModel_` — risque préexistant, non aggravé, à documenter.
- Projection = `setValueNotifyingHost` : garde « si différent » obligatoire.
- Sources (Image/Video/Camera/Sp3ctra + acqGate) : engines singletons à params globaux → réglages
  non chain-owned sans banking — hors-scope V1 (cf. C).

---

# Décisions à valider (recommandations incluses)

| # | Question | Décision |
|---|---|---|
| D1 | Mix RGB pour la color-FFT LuxSynth | Moyenne pondérée par intensity (vs RGB du 1er send) |
| D2 | Unifier le feed spectral LuxSynth côté cœur (aujourd'hui la FFT moteur vit dans l'UI → LuxSynth muet éditeur fermé) | **✅ VALIDÉ user 2026-07-11 : à faire** (chantier dédié, naturellement adossé à M4) |
| D3 | Loi de mix LuxWave | Bipolaire autour de 0.5 : `0.5 + Σ w·(line−0.5)` clampé |
| D4 | Staging silencieux (producteur muet sans no-signal) | Dernière valeur tenue, sans timeout (parité held-frame) |
| D5 | Multi-OUT même type dans UNE chain | Non en V1 (1 OUT/type/chain), ouvrir plus tard |
| A | Déplacement d'un module entre chains | **✅ VALIDÉ user 2026-07-11** : les réglages suivent l'instance (« fonctionnement intuitif ») ; la chain source garde son snapshot |
| B | Périmètre preset .sp3chain V1 | Params APVTS du manifest seulement (SCORE/media en V2) |
| C | Réglages des sources (engines singletons) | Hors-scope V1, à réévaluer si les sources deviennent multi-instances |

> Lancement de l'implémentation acté par le user le 2026-07-11 (« tu peux attaquer M2 »). Les
> recommandations D1/D3/D4/D5/B/C valent par défaut tant que non contestées.
