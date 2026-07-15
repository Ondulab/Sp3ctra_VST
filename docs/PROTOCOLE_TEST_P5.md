# Protocole de test P4/P5 — multi-instance sources + lecteurs score

Objectif : valider sur le device que la doctrine est tenue partout (tout module,
sur n'importe quelle chaîne, indépendant) après P5-M4/M5/M6 (commits `36d1050`,
`040ec0c`, `597a4e5`). Chaque test référence le risque qu'il couvre.
Cocher au fil de l'eau ; tout écart = noter le bloc + le geste exact.

**Setup de référence** : device SP3CTRA branché, audio 48 kHz, compteur
d'underruns visible, 2 chaînes minimum au rack.

---

## Bloc A — Un lecteur score, sémantique de base (parité legacy)

| # | Geste | Attendu |
|---|---|---|
| A1 | SCORE + →LUXSTRAL chaîne 1 ; GENERATE puis PLAY (page) | Lecture, tête avance, LED du bloc pleine ● |
| A2 | STOP en cours de lecture | Silence IMMÉDIAT (blanc) — aucune colonne qui sonne ; PLAY suivant repart de 0 |
| A3 | Loop OFF, laisser finir | Arrêt seul en fin, bouton PLAY repasse à l'arrêt, silence blanc |
| A4 | Loop ON / Reverse / vitesses 0.25×→4× | Boucle sans clic, inversion propre, vitesse audible ; le knob agit EN lecture |
| A5 | Scrub sur score ARRÊTÉ (drag sur la preview) | Son tenu qui suit le doigt ; relâche = silence ; PLAY suivant démarre à la colonne relâchée |
| A6 | En lecture : drag (seek) puis STOP immédiat puis PLAY | Repart de 0 — PAS de saut à l'ancienne position du drag *(fix seekHead)* |
| A7 | GENERATE pendant la lecture (reload EQ live) | La lecture reprend où elle en était (resume head) |

## Bloc B — N scores simultanés (le cœur de P5)

| # | Geste | Attendu |
|---|---|---|
| B1 | SCORE ch.1 + MIDI SCORE ch.2 (OUTs séparés) ; PLAY les deux | DEUX musiques simultanées, chacune sur SA chaîne, zéro fuite croisée |
| B2 | STOP du premier | Le second continue sans accroc ; chaîne 1 = silence blanc |
| B3 | PLAY/STOP au rack via la pastille LED de chaque module | Transport par instance ; pastille : ● joue / ◐ contenu / ○ vide (clic sans effet si vide) |
| B4 | + TIMBRE + VOICE (3-4 lecteurs en même temps) | Stable, 0 underrun |
| B5 | 2 scores sur 2 chaînes → LUXSTRAL toutes deux | ⚠️ Limitation CONNUE : le bandeau du haut peut strober entre les deux — l'AUDIO doit rester propre |

## Bloc C — Score + Sampler : indépendance (le relay n'existe plus)

| # | Geste | Attendu |
|---|---|---|
| C1 | Sampler joue ch.1 ; PLAY score ch.2 | Le sampler NE s'arrête PLUS (nouveau comportement voulu) — les deux jouent |
| C2 | STOP du score | Le sampler continue (il n'a jamais été interrompu) |
| C3 | Le sampler s'arrête (fin NONE / STOP) PENDANT que le score joue | Le score ne décroche PAS, pas de flash blanc ni dropout *(fix injectWhiteFrame)* |
| C4 | Séquenceur en marche avec steps VIDES + score en lecture | Pas de micro-dropout du score à chaque step vide *(même fix)* |
| C5 | REC sampler pendant qu'un score joue ailleurs | Capture sans bleed du score |

## Bloc D — Pages liées à l'instance (M5)

| # | Geste | Attendu |
|---|---|---|
| D1 | DEUX modules SCORE (2 chaînes) ; sélectionner l'un puis l'autre | La page suit : tête, état PLAY, cible du LOAD = l'instance sélectionnée |
| D2 | GENERATE+PLAY sur chaque instance | Contenus indépendants (chaque slot garde ses frames) |
| D3 | Retirer le module lié pendant que sa page est ouverte | Sa lecture s'arrête en silence ; la page retombe sur l'autre instance |
| D4 | Scrub en cours puis clic sur un autre module | Le scrub s'arrête proprement (pas de son tenu orphelin) |

## Bloc E — Affichage / device

| # | Geste | Attendu |
|---|---|---|
| E1 | Device EN STREAM + score en lecture | Waterfall/bandeau = le score, SANS scintillement avec le flux live *(fix gate udpThread)* |
| E2 | STOP du score | Affichage revient au flux (chaîne sourcée) ou au blanc (sans source) ; zone-1 cohérente (badge MODULE - CHAIN n) |
| E3 | Chaîne →LUXSYNTH sans source portant un score ; STOP | La vue SYNTH ne reste PAS gelée sur la dernière colonne *(fix teardown polyphonic)* |

## Bloc F — Sources média ×8 (re-check M3)

| # | Geste | Attendu |
|---|---|---|
| F1 | 2 IMAGE contenus différents sur 2 chaînes | Deux flux distincts dans les synthés |
| F2 | 2 VIDEO ; CAMERA par instance (si 2 devices) | Idem ; device caméra sélectionné/persisté PAR instance |
| F3 | LED des blocs média | Chaque bloc reflète SON état (plus le slot 0 partout) *(fix M5)* |

## Bloc G — Persistance

| # | Geste | Attendu |
|---|---|---|
| G1 | Sauver session (2+ scores chargés, médias) puis recharger | Transports audio STOPPED (never-auto-run), VIDEO SCROLL qui défile, médias rechargés ; frames score à régénérer (GENERATE) — historique |
| G2 | Presets de chaîne / .sp3s | Chargement sans crash ; mappings MIDI conservés (ids slot 0 legacy) |
| G3 | Lane d'automation `scorePlaying` dans le DAW | Pilote le module SCORE — et SEULEMENT lui (Timbre/Voice/MidiScore = transports UI) |

## Bloc H — Stress / RT

| # | Geste | Attendu |
|---|---|---|
| H1 | 30 min de jeu mélangé (scores + sampler + médias + FX) | 0 underrun, pas de dérive mémoire |
| H2 | CPU pendant lecture score | Un core actif en plus (busy-wait 1 kHz assumé — parité legacy) : vérifier le budget machine de scène |
| H3 | Ajout/retrait de modules À CHAUD pendant lecture | Pas de crash, silences propres, plan re-dérivé sans glitch audible |

---

Limitations connues (assumées, notées au plan) : strobe du tap A à 2 scores
LuxStral simultanés (viz par instance différée, comme les samplers) ;
speed/loop partagés entre lecteurs (params globaux) ; document de génération
UN par type de page ; 2 joueurs sur UNE MÊME chaîne = double-staging
(trou d'arbitrage documenté, préexistant).
