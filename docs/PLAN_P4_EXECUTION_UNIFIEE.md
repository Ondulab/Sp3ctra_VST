# PLAN — P4 : exécution uniforme des chaînes (un flux, un exécuteur)

> Établi le 2026-07-13, suite au bug « chaîne 2 MIDI SCORE idle joue le flux SP3CTRA de la
> chaîne 1 » (fix immédiat appliqué le même jour : no-signal **dynamique** + base blanche,
> non commité — voir État de départ). Ce plan résorbe la classe entière de bugs, pas le
> symptôme.
>
> **Doctrine d'exécution (contrat cible, formulé par le user)** :
> 1. Chaque chaîne est indépendante ; chaque module d'une chaîne est indépendant (instance
>    propre par chaîne).
> 2. L'ordre des modules compte : un module reçoit le flux du module d'au-dessus ; s'il est
>    actif il le **modifie** (transformateur) ou le **remplace** (player/source) ; le
>    résultat est consommé par les modules d'en dessous.
> 3. **UN seul flux par chaîne**, un seul mécanisme d'exécution — aucun module (Video
>    Scroll inclus) ne doit avoir de chemin de traitement à part.
> 4. Chaîne sans signal = papier blanc, jamais le flux live du device.
>
> Taxonomie des modules qui en découle (déjà presque vraie dans le plan) :
> **source** (émet le flux de base : SP3CTRA/IMAGE/VIDEO/CAMERA), **transformateur**
> (in→out : Pitch/Mask/Reverb/Echo/EQ), **sonde** (pass-through + capture : VideoScroll),
> **remplaceur** (pass-through inactif, remplace le flux quand il joue : SAMPLER,
> SCORE/TIMBRE/MIDI SCORE), **send** (pass-through + tap conditionné vers un moteur :
> → LUXSTRAL/LUXSYNTH/LUXWAVE).
>
> Contrainte physique assumée : une chaîne s'exécute à l'horloge de son producteur
> (udpThread au rythme des lignes UDP, feeder ~20-200 Hz, FramePlayerThread 1 kHz). Quand
> un remplaceur joue, il DEVIENT l'horloge du segment aval. Le multi-thread n'est pas
> l'écart ; l'écart est que chaque thread ré-implémente la marche différemment.

## État de départ (vérifié 2026-07-13, cartographie complète)

P3 (M1..M8) et Partie B (J1..J6) livrés : recettes uniformes `plan.chain[]`, staging +
mix pull pour les 3 moteurs, plan = unique autorité de routage. Fix du jour (non
commité) : `synth_chain_has_no_signal(sp, sig, score_playing)` dynamique
(multithreading.c:593) + base blanche `chain_white_line()` pour les chaînes
player-substituées sans source propre (multithreading.c:1887).

### Écart 1 — QUATRE marcheurs pour UN contrat
- `chain_execute_positional` (multithreading.c:720) — la référence : marche positionnelle
  complète (taps sélection :743, OUT staging :749-797, record sampler :798, probes via
  `image_chain_run` :818).
- `chain_shortcut_walk` (:833) — chaîne owner du bus modulé / relay score : ne marche PAS
  les processeurs, observe base/mod par position, skip conditionnel des probes aval.
- `chain_run_premarker_segment` (:905) — chaîne player-owned, segment amont seulement.
- `chain_run_inserts_with_viz_tap` (:666) — sous-plans processeurs (build modulé idle +
  post-marqueur player via `chain_apply_post_marker_inserts` :1037).
Plus les auxiliaires qui n'existent que pour recoller ces morceaux :
`chain_build_sampler_premarker_plan` (:1176), `publish_viz_tap_sampler_shortcut` (:1140,
tap « approximé » avant/après marqueur), flags `skip_post_marker_probes` /
`premarker_tap_done` / `mod_is_chain_stream`.
Conséquence directe : VideoScroll est alimenté par **5 sites** (image_chain.c:149 via les
marches ; multithreading.c:636 sweep blanc ; :853 shortcut ; :939 pré-marqueur) — c'est
le « traitement différent » perçu par le user.

### Écart 2 — Le bus « modulated » global
Un canal UNIQUE possédé par « la » chaîne sampler (owner = moteur jouant, sinon 1re
chaîne sampler — :1608-1628), construit HORS boucle uniforme (udp :1683-1777, feeder
:2226-2253), consommé par `chain_shortcut_walk` comme flux de routage. Consommateurs
display : CisVisualizer MODULATED (« CHAIN 1 ») CisVisualizerComponent.cpp:857 ;
VideoDisplayComponent.cpp:81 (compteur `lines_modulated` = fraîcheur) ;
`lux_sampler_on_modulated_frame_ready` (miroir snapshot + REC). Source du bug M6b et des
gardes fragiles (`mod_is_chain_stream`).

### Écart 3 — Exécution player cousue entre threads avec DUPLICATION
Pendant la lecture : udp/feeder exécutent [0, marqueur) (`chain_run_premarker_segment`),
FramePlayerThread exécute (marqueur, N] à 1 kHz — mais avec SES PROPRES chemins :
`ls_sends_stage_player_frame` (multithreading.c:175, copie privée par send + inserts
post-marqueur re-appliqués par send), `lx_send_stage_player_frame` (:1000),
`chain_player_apply_synth_a_inserts` (:1075, lit plan.synth[0]),
`chain_player_record_downstream` (:120). Cinq implémentations du même segment aval.

### Écart 4 — Le chemin legacy (3e régime d'écriture)
`pipeline_process_frame` + commit struct-entière gaté `s_udp_frame_live_pp`
(multithreading.c:1980-1986, commit :2061-2070) : en topologie SANS send → LUXSTRAL, le
moteur LuxStral est encore nourri du pipeline live — contradiction avec la doctrine
« nourri UNIQUEMENT via OUT » (LuxSynth/LuxWave l'appliquent déjà depuis P3-M3).
`plan.synth[]` survit pour 3 consommateurs : résolution source legacy (:1789, code retour
IGNORÉ — dernier user du fallback live), `chain_player_apply_synth_a_inserts` (:1082),
gating freeze CisVisualizer (CisVisualizerComponent.cpp:765-769). S'y ajoute le chemin
`image_chain_process_inserts`/insert-taps slot-0 (:1743) dans le build modulé.
⚠️ Découverte cartographie : le mixer audio ne commit PAS `stereo.pan_positions`
(multithreading.c:2696-2715) alors que les workers les lisent
(synth_luxstral_threading.c:645) — valeurs figées du dernier commit legacy. À traiter M1.

### Écart 5 — Les pointeurs fallback live de `synth_source_base`
Sur -1 (« no signal ») les pointeurs de sortie pointent ENCORE sur le live
(multithreading.c:560-566, « legacy pointer users »). Chaque site d'appel doit y penser —
c'est exactement le piège qui a produit le bug du 2026-07-13. Dernier consommateur
légitime du fallback : le site legacy :1789 (meurt en M4).

---

# Jalons

## M1 — Hygiène : fallback BLANC + pan_positions (S)

1. `synth_source_base` : sur retour -1, `out_*` pointent sur `chain_white_line()` (plus
   jamais le live). Le site legacy :1789 est le seul à dépendre du fallback live → le
   gater explicitement (`if (sig < 0)` → skip compute + commit, publier tap blanc) en
   attendant sa mort en M4. Retirer la substitution blanche locale ajoutée le 2026-07-13
   (devenue redondante), garder le test no-signal dynamique.
2. Bug `pan_positions` : décider (mesure) si les workers les utilisent quand
   `stereo_valid` (left/right_gains normalisés suffisent ?) ; si oui, les committer dans
   le mixer ; si non, purger la lecture worker.
   **→ RÉSOLU 2026-07-14 : pas de bug audible.** Le rendu ne lit QUE
   `precomputed_left/right_gain` (synth_luxstral_threading.c:554-555) ; la copie
   `precomputed_pan_position` était morte → purgée (champ + alloc + memcpy + free).
   Le champ struct `stereo.pan_positions` reste écrit-jamais-lu (writer
   image_pipeline.c:439) → purge en M5 avec la signature d'img_stage_stereo_pan.
3. Commit du fix fuite du 2026-07-13 avec ce jalon.

Validation : matrice no-signal (chaîne MIDI SCORE/SCORE/SAMPLER idle → silence + scroll
blanc, device streaming et feeder) ; parité sonore topologies à sends ; stéréo LuxStral
inchangée à l'oreille.

## M2 — UN exécuteur : `chain_execute_span()` (L — cœur du chantier) — ✅ FAIT (2026-07-14)

Statut : implémenté, build vert, revue 8-angles passée (1 régression trouvée et
corrigée avant commit : write-back d'affichage = flux de FIN de chaîne via
`ChainExecOut.end*`, pas le flux au premier OUT). Réalisation effective :
- `chain_execute_span(sp, chain, from, to, ctx, in, out)` = LE marcheur unique ;
  `ChainExecCtx` (scratchs par thread, `rec_mode` NONE/IDLE/INPUT,
  `rec_skip_engine`, `player_fed`, `force_play`, `publish_tap_at_end`) ; inits
  désignées aux 5 sites (zéro-fill C99, plus de memset copy-paste).
- Split-point UNIQUE : `chain_owning_marker_pos(sp, score_owns, engine_slot)`
  partagé producteurs (-1 = tout moteur driving) / players (moteur exact).
- Producteurs : TOUTE chaîne player-owned (LS OUT ou non) = span [0, marqueur]
  chez udp/feeder (probes/FX amont au line rate, OUTs au-dessus du marqueur
  stagés ICI — avant : orphelins —, marqueurs = record INPUT) + tap blanchi si
  chaîne sans source ; le player marche (marqueur, N].
- Player : `chain_player_execute_owned` remplace les 5 chemins
  (ls_sends_stage_player_frame, lx_send_stage_player_frame,
  chain_player_apply_synth_a_inserts, chain_apply_post_marker_inserts,
  chain_player_record_downstream) — UNE marche par chaîne possédée : staging de
  CHAQUE OUT à sa position, FX/probes tickés UNE fois (avant : double-tick FX +
  double-capture probes entre marqueur et OUT via copies par send + run
  in-place), record downstream à la position du marqueur, taps exacts ;
  scratchs `_Thread_local` (A/B simultanés).
- VOICE : famille score factorisée (`kScoreFamily`/`isScoreFamily`,
  ModuleCatalog.h) — le bug « chaîne VOICE seule jamais player-owned » venait
  d'une 4e liste manuscrite désynchronisée ; les 3 sites processeur itèrent la
  liste.
- Morts : chain_execute_positional, chain_run_premarker_segment + les 5 chemins
  player. RESTENT pour M3 : chain_shortcut_walk, publish_viz_tap_sampler_shortcut,
  chain_build_sampler_premarker_plan, chain_run_inserts_with_viz_tap (build
  modulé idle).
Deltas assumés (doctrine « le module reçoit le flux d'au-dessus ») : le record
downstream capture le flux À SA POSITION (post-opacité/blend/FX amont — avant :
playback brut pré-mix) et s'arrête au transport STOP ; le staging LuxSynth/Wave
player voit SA chaîne à SA position (avant : premier OUT trouvé, frame cross-FX) ;
OUT au-dessus du marqueur stagé au rythme producteur (avant : jamais pendant la
lecture). Trou PRÉ-EXISTANT documenté (non traité, → M3/arbitre) : score jouant
+ sampler driving sur UNE chaîne (moteurs sans partage) = double-staging.
À VALIDER en réel (matrice M5) : sampler A+B cross-chaîne, REC pendant play,
score/MIDI SCORE/VOICE relay, chaîne sans source, 0 underruns.

### Plan initial (référence)

Généraliser `chain_execute_positional` en marcheur de SEGMENT, seul code au monde qui
sait exécuter une chaîne :

```c
/* Marche les positions [from, to) de la recette sp sur le flux in_*.
 * Gère TOUS les ids à leur position exacte : tap sélection, OUT staging
 * (ls/lx/lw), record sampler (input du marqueur), probes via image_chain_run,
 * marqueurs pass-through. S'arrête à un remplaceur ACTIF (retourne sa position)
 * — le segment aval appartient à l'horloge du player. */
static int chain_execute_span(const SynthChainPlan *sp, int chain_idx,
                              int from, int to, ChainExecCtx *ctx,
                              const uint8_t *inR, const uint8_t *inG,
                              const uint8_t *inB, int nb_pixels,
                              ChainExecOut *out);
```
`ChainExecCtx` = { viz bus, pp/lx scratchs du thread appelant, allow_sampler_record,
player_fed, pb_marker_id }. Appelants et équivalences :
- udp/feeder, chaîne sans player actif : span [0, N] — remplace
  `chain_execute_positional` (comportement identique, refactor mécanique).
- udp/feeder, chaîne player-owned : span [0, pos_marqueur] (le marqueur record son input
  = `lux_sampler_record_input_frame`, puis stop) — remplace
  `chain_run_premarker_segment` ET le build modulé pré-marqueur (le sous-plan
  `chain_build_sampler_premarker_plan` meurt : on marche la VRAIE recette, les probes
  amont sont capturés ici et NULLE PART ailleurs).
- FramePlayerThread : span (pos_marqueur, N] avec base = frame de lecture — remplace
  `ls_sends_stage_player_frame` (les copies privées par send disparaissent : UNE marche
  de la chaîne stage chaque OUT à sa position), `lx_send_stage_player_frame`,
  `chain_player_apply_synth_a_inserts` et `chain_apply_post_marker_inserts`.
  `chain_player_record_downstream` est absorbé : un marqueur SAMPLER aval EST un insert
  du span (record de son input, 1:1 au tick player).
- Le tap sélection est publié DANS le marcheur à sa position exacte partout →
  `publish_viz_tap_sampler_shortcut` (approximation avant/après marqueur) meurt.
- `chain_shortcut_walk` meurt (voir M3). `chain_run_inserts_with_viz_tap` meurt.

Invariants à préserver (tests de parité) :
- FX statefuls (Reverb/Echo/EQ) tickent EXACTEMENT une fois par ligne et par chaîne.
- Un probe est capturé une seule fois par frame (plus de skip-flags croisés).
- Staging : mêmes producteurs qu'aujourd'hui (udp/feeder idle, player en lecture),
  single-writer par slot de chaîne conservé.
- RT : zéro allocation, O(num_inserts), scratchs par thread inchangés.

Validation : parité bit-exact en idle (chaîne simple, multi-OUT, probe après OUT) ;
lecture sampler A+B cross-chaîne (2026-07-13) intacte ; REC pendant play = resampling ;
score relay ; CPU udpThread ≤ actuel (rt_profiler).

## M3 — Mort du bus « modulated » + bascule D2 — ✅ FAIT (2026-07-14)

Statut : implémenté, build vert, revue 2 angles (cross-file : zéro référence
restante ; comportements supprimés : 5/6 invariants recouvrés, 1 gel visuel
pré-existant corrigé — le span pré-marqueur publie le tap A quand il stage un
OUT au-dessus du marqueur). Réalisation effective — AU-DELÀ du plan initial
(D2 validée « bascule immédiate ») :
- Bus MODULATED SUPPRIMÉ intégralement (pas juste rétrogradé) : champs +
  snapshot/get + compteur retirés d'AudioImageBuffers ; builds dédiés udp
  (idle + PLAYING re-copy) et feeder supprimés ; TOUTE chaîne s'exécute par la
  marche uniforme (le marqueur SAMPLER enregistre à sa position,
  lux_sampler_record_chain_frame). Morts : chain_shortcut_walk,
  publish_viz_tap_sampler_shortcut, chain_build_sampler_premarker_plan,
  chain_run_inserts_with_viz_tap, lux_sampler_on_modulated_frame_ready,
  onModulatedFrameReady, mirrorSamplerSnapshot, onFrameAssembled (+ hook
  legacy), image_chain_process_inserts + tap-demand + ordre GLOBAL
  (image_chain_set_order — l'ordre par chaîne vient de la recette),
  insert-taps d'AudioImageBuffers.
- D2 : zone-1 contextuelle PARTOUT — sources/SAMPLER/famille score/SEQUENCER
  → SELECTED_TAP (badge « MODULE - CHAIN n ») ; les vues RAW/LIVE/MODULATED ne
  sont plus atteignables (enum conservé pour la persistence, purge + migration
  → M5).
- Fraîcheur waterfall : `frame_seq` (fetch_add multi-producteurs) bumpe à
  chaque publish de tap moteur — remplace lines_modulated.
- Restes pour M5 : snapshot sampler (écrit, plus aucun lecteur), enum
  VisualizerMode legacy, param chainInsertOrder (projection inerte).

### Plan initial (référence)

Le bus cesse d'être un canal de ROUTAGE ; il devient une publication d'affichage émise
par le marcheur au marqueur SAMPLER de la chaîne owner (idle) — le player la publie déjà
en lecture (LuxSampler.cpp:3120).
1. Builds modulés dédiés supprimés (udp :1683-1777 idle-branch, feeder :2226-2253) : la
   marche uniforme [0, marqueur] fait foi ; au marqueur : hook unique
   `lux_sampler_on_chain_frame(engine, stream…)` = miroir snapshot (si owner display) +
   REC armé (`lux_sampler_record_chain_frame` existant) + publication
   `snapshot_modulated` (compat display V1).
2. La branche udp PLAYING qui re-copie le snapshot sampler (:1657-1680) meurt (fraîcheur
   `lines_modulated` déjà assurée par le player).
3. `mod_owner_chain`/`first_sampler_chain`/`mod_is_chain_stream` réduits au seul choix
   du owner DISPLAY (une ligne), plus aucune influence sur le routage.
4. Chemin legacy `image_chain_process_inserts` + insert-taps slot-0 (:1743,
   image_chain.c:53-113) : suppression — la vue Pitch/Mask passe sur le tap de sélection
   (fallback RAW conservé), `s_tap_demand`/`need_modulated` purgés.
V2 (hors P4, mémoire « viz per-sampler différée ») : MODULATED → vue par moteur.

Validation : vue « CHAIN 1 » vivante en idle/REC/PLAY/score ; waterfall sans freeze au
play/stop ; REC idle depuis IMAGE (feeder) et device (udp) ; badge zone-1 exact sur
module aval pendant lecture.

## M4 — Purge legacy : le mixer est LE SEUL écrivain (M)

1. Le bloc mixer audio (multithreading.c:2649-2724) tourne INCONDITIONNELLEMENT en
   VST : `num_ls_sends == 0` → commit silence (sections additive/stereo/strokeforge à
   zéro, tap moteur blanc une fois). **Décision D1** : sans module → LUXSTRAL, LuxStral
   est muet (doctrine no-OUT = no-feed, déjà vraie pour LuxSynth/LuxWave).
2. Mort du chemin legacy VST : `s_udp_frame_live_pp`, commit struct-entière (:2061-2070),
   résolution source :1789, publish tap :1997, `lux_sampler_is_passthrough()` comme gate
   de commit. `pipeline_process_frame` reste pour le standalone non-VST (#ifndef) et le
   player (`pipeline_path_luxstral` par send — inchangé).
3. `plan.synth[]` : plus aucun consommateur runtime après M2 (player) et M4.2 (legacy) —
   reste CisVisualizer (gating freeze :765-769) → basculer sur la recette
   `ls_send[sélectionné]`/`chain[]`. Puis cesser de remplir `synth[]`, re-keyer le
   scratch `s_synth_src_scratch` sur les seuls index de chaîne.

Validation : rack vide → silence total ; 1 send intensity 1 → parité ; topologies
display-only (probe sans OUT) → visuels vivants, moteurs muets ; standalone non-VST
inchangé.

## M5 — Purge finale + matrice réelle (S)

Suppression des morts (les 4 marcheurs remplacés, auxiliaires, flags), commentaires/
headers (chain_plan.h, image_chain.h « ONLY consumed by image_chain_run », doc
audio_image_buffers.h:60 « mirrored by the video waterfall » déjà fausse), mise à jour de
ce doc + PLAN_P3. Matrice de validation MANUELLE (device réel) : live UDP ; IMAGE/VIDEO/
CAMERA ; sampler idle/REC/PLAY/STEP, A+B cross-chaîne ; SCORE/TIMBRE/MIDI SCORE relay +
resume ; chaîne sans source = silence/blanc ; hot-swap device↔feeder ; 0 underruns ;
CPU udpThread ≤ baseline.

---

## Risques

1. **Parité REC** : les points de capture (input du marqueur, downstream 1:1) doivent
   rester identiques — c'est le contrat 2026-07-13 ; testé par la matrice sampler.
2. **Cadence stagings** : le player stage désormais via le marcheur — même fréquence
   (1 kHz), mêmes slots ; le single-writer par chaîne est structurel (une chaîne = un
   producteur par segment).
3. **CPU udpThread** : la marche uniforme remplace des shortcuts — mais elle exécute la
   même liste d'inserts ; le seul surcoût potentiel est la disparition des sous-plans
   filtrés (négligeable, O(N) sur ≤ 20 entrées). Mesure rt_profiler à chaque jalon.
4. **D1 (no-OUT = silence LuxStral)** : changement de comportement audible pour les
   vieilles sessions sans module → LUXSTRAL. Assumé doctrine ; à annoncer.
5. **Régression relay score** : la mécanique relay (scoreRelaySlot_, resume) n'est PAS
   touchée — seuls les chemins d'exécution changent. Matrice M5 obligatoire.

## Décisions à valider

| # | Question | Décision |
|---|---|---|
| D1 | Sans → LUXSTRAL posé, moteur LuxStral muet ? | **✅ VALIDÉ user 2026-07-13 : OUI** (doctrine no-OUT = no-feed, aligne les 3 moteurs) |
| D2 | Vue zone-1 MODULATED pour source/sampler/score | **✅ VALIDÉ user 2026-07-13 : bascule IMMÉDIATE** sur le tap de sélection positionnel (badge « MODULE - CHAIN n » partout) — intégrée à M3 (le bus modulated perd son dernier consommateur zone-1 ; restent le compteur de fraîcheur waterfall et le miroir snapshot sampler) |
| D3 | `pan_positions` non committés par le mixer | Mesurer en M1, corriger ou purger |
| D4 | Snapshot sampler global (owner-gated) | Conservé V1 ; per-sampler viz différée (déjà acté) |

> Ordre d'exécution : M1 → M2 → M3 → M4 → M5, chacun build vert + smoke standalone avant
> le suivant. M2 est le gros morceau ; M3/M4 sont surtout des suppressions une fois M2 en
> place. Lancement acté par le user le 2026-07-13 (« lance M1 »).
