# PLAN P6 — SAMPLER ×8 (moteurs multi-instance comme tout le rack)

**Demande user (2026-07-15)** : « 4 ce serait déjà bien, si on peut aller à 8
comme tous les autres ça serait parfait. » Le SAMPLER est le dernier module
plafonné à 2 instances (moteurs A/B) alors que P5 a mis médias et famille
score à 8. Objectif : **8 moteurs sampler**, lecture simultanée sur chaînes
séparées, et une architecture de lecture qui tienne en scène.

## État des lieux (2026-07-15)

Bien plus prêt que prévu — les formats et registres sont déjà indexés moteur :
- `FrameSequencer::kMaxSamplers` = **déjà 8** (il ne manque que le câblage).
- `SamplerPageComponent::setSamplerIndex(slot)` **déjà poussé à la sélection**.
- Persistance `SAMPLER_SLOTS` **déjà keyée par idx moteur** (compat ascendante).
- Slots à allocation paresseuse → 8 moteurs vides ≈ gratuits.
- Le churn : `kMaxEngines=2` (registre), `kMaxSamplerEngines=2` (modèle),
  membres processor `luxSampler`/`luxSamplerB` (~26 sites), banques params
  moteur (6 params ×: MidiChannel/OctaveOffset/MaxDuration/RecMode/PlayMode/
  NumBanks), arbitre `s_enginesShareChain` (bool GLOBAL A/B → matrice par
  paire), `jlimit(0,1)` au marqueur du plan, `engines[2]` page sampler,
  routage `startsWith("luxSamplerB")`.
- ⚠️ Threads : 1 FramePlayerThread PAR moteur, busy-yield 1 kHz en lecture →
  8 lectures = 8 cœurs. Le modèle cible est le ScorePlayerService (P5-M4) :
  UN thread multiplexé.

## Jalons

### P6-M1 — 8 moteurs fonctionnels (L) — **FAIT (2026-07-15)**
`kMaxEngines`/`kMaxSamplerEngines` → 8 ; processor `samplers_[8]`
(getLuxSampler()=moteur 0, getSampler(i) 0..7) ; banques params moteurs 2-7
(`fsEngineParam` : 0="luxSampler*", 1="luxSamplerB*" legacy, n≥2=
"luxSampler{n}_*") + listeners + restore + routage parameterChanged +
MIDI-learn targets ; marqueur plan `jlimit(0,7)` ; **arbitre par paire**
(masque de partage de chaîne publié au derive, `stopOtherEnginesPlayback`
n'évince que les moteurs partageant la chaîne) ; séquenceur câblé ×8 ;
persistance SAMPLER_SLOTS ×8 ; page sampler `engines[8]` ; enable par moteur
×8 ; MIDI channel défaut = n+1. Threads : encore 1/moteur (M3).

> **Réalisation.** kMaxEngines/kMaxSamplerEngines = 8 ; processor
> `samplers_[8]` (accès `getSampler(i)` clampé, getLuxSampler() = moteur 0) ;
> banques 2-7 générées ("luxSampler{N}_*", 6 params, MIDI ch. défaut N+1) +
> listeners/restore/routage en boucles ; parse moteur dans parameterChanged et
> les cibles MIDI-learn ("smp:e{N}" générique + "luxSampler{N}_") ; arbitre
> par PAIRE : `s_shareMask[8]` publié au derive (chaîne par chaîne, bitmask
> des moteurs co-hébergés), `stopOtherEnginesPlayback` n'évince que les
> moteurs partageant la chaîne ; marqueur plan clampé 0..7 ; séquenceur câblé
> ×8 (gate par moteur en tableau) ; SAMPLER_SLOTS ×8 (format idx-keyé,
> compat) ; **fix aliasing** : pulses d'actions MIDI (smpRec/Play/Save/Clear/
> EqPending) étaient `[2]` masqués `e&1` — les moteurs 2-7 auraient déclenché
> REC/PLAY sur 0/1 ; dimensionnés kMaxEngines + clamp des deux côtés.
> ⚠️ .sp3s reste v2 (moteurs 0/1) jusqu'à M2 — TODO posé dans le code.

### P6-M2 — UI + .sp3s ×8 (M)
Page sampler et SETUP par instance sélectionnée (mécanisme déjà en place —
étendre les bornes) ; .sp3s : engines ×8 (format idx-keyé, compat v2 A/B) ;
LED rack par moteur (déjà per-slot depuis P5) ; strips AUDIO MIX si concernés.

### P6-M3 — Lecture multiplexée (L — la vraie condition du ×8 en scène)
UN SamplerPlayerService (modèle ScorePlayerService) : les sessions
(voices + commandes + white-frame) deviennent des états PAR MOTEUR tickés par
UN thread 1 kHz lock-step ; FramePlayerThread devient un objet de session
sans thread. Coût CPU plat quel que soit le nombre de moteurs en lecture.

### P6-M4 — Matrice device (S)
4 puis 8 samplers en lecture simultanée sur chaînes séparées, REC pendant
lectures multiples, séquenceur, éviction même-chaîne uniquement,
presets/reload/.sp3s, 0 underrun.

## Décisions
| # | Question | Décision |
|---|---|---|
| D1 | Cap | 8 (aligné P5) — la généralisation coûte pareil que 4 |
| D2 | Threads | M1 = 1/moteur (inchangé) ; M3 = multiplexage obligatoire avant de déclarer le ×8 « scène-ready » |
| D3 | Ids params | moteurs 0/1 = ids legacy (sessions/mappings intacts), 2-7 = "luxSampler{n}_*" |
| D4 | Éviction | par paire partageant une chaîne (matrice au derive) — fini le bool global A/B |
| D5 | REC | inchangé par moteur (marqueur armé) — N REC simultanés possibles comme aujourd'hui |
