# PLAN — Sp3ctra Link : refonte du dialogue CIS ⇄ VST (retrait RTP-MIDI, protocole propriétaire, MIDI fabriqué dans le VST, retour LEDs + OLED)

**Date : 2026-08-30 — Chantier transverse firmware `Sp3ctra_CIS_Firmware` + `Sp3ctra_VST`**

> **État au 2026-08-30** — proposition d'architecture, rien d'implémenté.
> Les décisions D1 à D14 sont des recommandations à valider (ou amender)
> avant le jalon V0 ; la liste courte des points à trancher est en §11.

## 1. Contexte et objectif

Demande :

1. **Retirer RTP-MIDI** (AppleMIDI) du CIS — résultat insatisfaisant — et
   passer par un **format Ethernet propriétaire** entre le CIS et le VST.
2. **Boutons + accéléromètre + gyroscope = contrôles MIDI affectables** : le
   canal / numéro MIDI de chaque bouton / axe se règle **dans le module
   SP3CTRA du VST**, et c'est **le VST qui fabrique le MIDI** à partir des
   données brutes du CIS (plus aucun MIDI produit par le firmware).
3. **Protocole d'initialisation** CIS ⇄ VST : détection / présence / prise en
   main, puis **accord sur le contenu des trames UDP** (et d'éventuels autres
   canaux), pour pouvoir supporter de **futurs devices** aux caractéristiques
   différentes.
4. **Retour VST → CIS** : rétro-éclairage des boutons et **écran OLED** ; le
   VST peut **surcharger l'affichage** avec un ou plusieurs paramètres en
   cours de modification, pour se passer de l'écran de l'ordinateur quand une
   chaîne contient le module IN SP3CTRA.

## 2. État des lieux (inventaire)

### 2.1 Firmware (STM32H745, CM7 = réseau/CIS/IMU, CM4 = OLED/boutons/LEDs)
- **Flux image UDP** (`CM7/Peripheral/Src/udp_client.c`) : un `netconn` UDP
  **connecté** à `shared_config.network_dest_ip:network_udp_port` (statique,
  défaut `192.168.100.10:55151`) ; 12 fragments de 288 px par ligne
  (`packet_Scanline`, 880 o, envoyés **en ordre inverse**), puis un
  `packet_IMU` (56 o) par ligne ; l'IMU est **rééchantillonnée une fois par
  ligne** (`icm42688_TIM_Callback()` appelé depuis `udpClient_sendPackets`).
  Aucun handshake ni heartbeat : `udpClient_sendStartupInfoPacket()` est du
  code mort. Pas de champ magic/version dans l'en-tête ; `type` est un **enum
  de 4 octets** ; à 200 DPI `total_fragments=6` mais les tampons sont
  dimensionnés pour 12.
- **RTP-MIDI** : `rtpmidi_session.c` (1111 l.), `rtpmidi_packet.c` (276 l.),
  `rtpmidi.h`, `midi_button_mapper.c` (boutons → CC 20/21/22 ou notes,
  configurable via web), `midi_led_mapper.c` (CC 30/31/32 → LEDs, avec un
  **bug de signature** : `rtpmidi_rx_callback_t(status,d1,d2)` mais le mapper
  lit arg0 comme un canal, donc un NoteOn est traité comme un CC), `midiTask`
  (1 ms) dans `freertos.c`, mDNS `_apple-midi` dans `lwip.c`, options
  `rtpmidi_mode` / `rtpmidi_control_port` / `midi_button_*` dans
  `shared_config` + `CONFIG.TXT` + `config.html` + endpoints HTTP.
  **L'IMU n'est jamais envoyée en MIDI.**
- **Boutons** (CM4, `gui_interaction.c`) : 3 boutons, anti-rebond 20 ms,
  publication `shared_var.button_events[i] = {state, pressed_time(=0 toujours),
  sequence_number}`. Aucun menu piloté par les boutons.
- **LEDs** (CM4, `leds.c`) : 3 LEDs **mono**, PWM logiciel 10 kHz (TIM12),
  fenêtre de comparaison `system_time % 100` (échelle **0..100**) ; modèle
  d'animation à 2 phases `{brightness_1,time_1,glide_1, brightness_2,time_2,
  glide_2, blink_count}` déjà implémenté ; le bouton pressé **force** sa LED
  allumée. Le mapper MIDI CM7 écrit `value*1000/127` → **sature** dès CC ≥ 13.
- **OLED** SSD1362 **256×64, 16 niveaux**, bus FMC, **propriété exclusive du
  CM4** ; framebuffer + carte de pixels modifiés (`ssd1362_writeUpdates`) ;
  polices 8×8 / 16×16 / 16×32 ; zones : waterfall CIS (pleine hauteur) +
  bandeau IMU optionnel 17 px ; popup config 3 s (`gui_displayPopUp`).
- **IMU** ICM-42688 (CM7, SPI2+DMA, ODR 1 kHz) : `acc` retourné en **m/s²**
  (×9.81) alors que le commentaire de `udp_client.c` dit « en g » ; `gyro` en
  dps ; `integrated_*` **jamais écrits** (zéros sur le fil).
- **Identité** : `FW_VERSION "3.12.1"` ; MAC **codée en dur**
  `00:80:E1:00:00:00` (identique sur toutes les unités) ; hostname mDNS fixe
  `sp3ctra` ; **pas de numéro de série** ; seul le SSRC RTP-MIDI dérive de
  `HAL_GetUIDw0/1/2()`.
- **Réception PC → CIS** : HTTP maison (port 80, `http_server.c` 1712 l.,
  `strncmp`/`sscanf`), FTP 21 ; `tcp_client.c` (LEDs, port 5000) **jamais
  initialisé** (mort). IP **statique uniquement**, pas de DHCP ni découverte.
- **Mémoire partagée** : `shared_var` / `shared_config` (`0x24000000`,
  2 Ko), `packet_IMU` (`0x24000800`), `scanline_CM4[12]` (SRAM4
  `0x38000000`, copié par MDMA + HSEM #1 à chaque ligne).
- lwIP : `MEMP_NUM_NETCONN 16`, `MEMP_NUM_UDP_PCB 8`, `MEMP_NUM_NETBUF 16`,
  checksums UDP **désactivés** (gen + check), `LWIP_SO_RCVTIMEO 1`.
- Repo firmware : branche `master`, **10 fichiers modifiés non commités**
  (portage CMSIS v2 du 2026-08-29, testé) — à commiter avant ce chantier.

### 2.2 VST (`Sp3ctra_VST/vst/source`)
- **Réception** : thread C `udpThread` (`threading/multithreading.c`
  L1424-2098) sur une socket BSD (`communication/network/udp.c`), écoute
  `INADDR_ANY:55151`, adresse multicast par défaut `239.100.100.100`
  (`SP3CTRA_DEFAULT_UDP_ADDRESS`), `SO_RCVTIMEO 100 ms`. Parse
  `IMAGE_DATA_HEADER` (réassemblage par bitmap de fragments, ligne incomplète
  = jetée, `packet_id` ignoré) et `IMU_DATA_HEADER` (→ `Context::imu_*`,
  **aucun lecteur** dans tout le VST). `BUTTON_DATA_HEADER` /
  `LED_DATA_HEADER` / `STARTUP_INFO_HEADER` déclarés, **jamais traités**. Le
  tampon de réception est `sizeof(packet_Image)` pour tous les types.
- **Rien n'est envoyé au CIS en UDP** (aucun `sendto`). Aucune découverte.
  **Aucun code RTP-MIDI** côté VST : les boutons arrivent par la session
  MIDI réseau CoreMIDI de macOS → entrée MIDI de l'hôte → `processBlock`.
- **HTTP** : `communication/device/Sp3ctraDeviceClient` (ThreadPool 1,
  espacement 150 ms entre GET car « la pile lwIP du device a peu de PCB et
  doit continuer à streamer ») + `ui/setup/SourceSetupPanel` (SETUP du bloc
  SP3CTRA, `kPreferredH = 1290`) : LINK (IP device, port/adresse UDP) → CIS →
  IMU → GUI → **MIDI CHANNELS (SW1-SW3)** → NETWORK (**mDNS, RTP-MIDI mode,
  ports MIDI**) → FIRMWARE.
- **Module SP3CTRA** : `ModuleCatalog.h` L99 (`ModuleType::Sp3ctra`, rôle
  Source, pas de param d'enable → LED du rack = transport `imageFreezeMode`),
  `ChainBlockId::Chain1Source/Chain2Source`, page PLAY
  `image/SourcesTabComponent.h` (transport + fade + vitesse d'acquisition ;
  n'utilise pas `ModuleChrome`), faces PLAY | SETUP (`blockHasSetup` L1007,
  `layoutZone3` L2069/2096). `FaceSwitchBar` a un **mode segments
  personnalisés** (`setCustomSegments`, `onSegmentSelected`) introduit pour
  VIDEO SCROLL.
- **MIDI** : `midi/MidiMappingEngine` (128 slots lock-free, `processMidi`
  sur le thread audio, MIDI-learn = premier événement « press » sur n'importe
  quel canal, `IVirtualMidiSink` pour les cibles hors APVTS, `touchGen_` /
  `takeLastTouchedParam` pour le MIDI-follow) ; `processBlock`
  (`PluginProcessor.cpp` L3247) : `buffer.clear()` →
  `midiMap_.processMidi(midiMessages)` → … lecteurs de notes (PITCH, MASK,
  LuxSynth, LuxWave) → `drainMidiTapToBus` **en dernier**. Il n'existe **aucun
  chemin d'injection MIDI interne**. `MidiTapSink` sait ouvrir un port MIDI
  de sortie virtuel.
- **Persistance** : 9 params de lien (`udpPort`, `udpByte1..4`,
  `deviceIpByte1..4`) + `sensorDpi` dans l'APVTS **et** forcés par
  `session/MachinePrefs` (machine-scoped) ; la config device n'est jamais
  stockée dans le VST.
- **Singleton** `Sp3ctraSharedCore` (process-wide) possède la socket, les
  tampons et les threads : tout nouveau canal doit y vivre, pas dans
  `PluginProcessor`.

## 3. Décisions de conception

### D1 — Un seul protocole propriétaire : **Sp3ctra Link (SLP) v1**, UDP, deux canaux
| Canal | Sens | Port | Contenu |
|---|---|---|---|
| **CONTROL** | bidirectionnel | device écoute **55150** ; le VST répond/émet depuis un port éphémère, le device répond à l'adresse:port source | HELLO/ANNOUNCE, BIND/BIND_ACK, UNBIND, PING/PONG, LED_SET, OLED_OVERLAY/CLEAR, CFG_*, CAL_START, ERROR |
| **STREAM** | device → VST | port choisi par le VST dans BIND (défaut **55151**, inchangé) | LINE (fragments image), HID (boutons + IMU) |

Deux ports plutôt qu'un : le chemin temps réel du VST (`udpThread` C,
`recvfrom` → réassemblage → chaîne) reste intact ; le contrôle vit dans un
thread JUCE séparé. Côté CIS, `cis_sendTask` garde son `netconn` connecté,
le serveur de lien a le sien. RTP-MIDI, `_apple-midi`, `tcp_client`, le
`midiTask` et les mappers MIDI sont **supprimés** du firmware ; le VST perd
la session MIDI réseau macOS (plus rien à configurer dans *Audio MIDI Setup*).

### D2 — Contrat de fil = **un en-tête C partagé**, octet pour octet
`sp3ctra_link.h` (C pur, `#pragma pack(1)`, little-endian, largeurs fixes,
`static_assert` sur chaque `sizeof`) copié **à l'identique** dans
`Sp3ctra_CIS_Firmware/Common/Inc/` et
`Sp3ctra_VST/vst/source/communication/link/` ; un script
`scripts/check_link_header.sh` (VST) compare les deux SHA-256. Plus d'enum
sur le fil, plus d'`aligned(4)` pris pour du packing. Tout message commence
par :

```c
struct slp_hdr {            /* 12 octets */
    uint16_t magic;         /* SLP_MAGIC 0x5333 ("S3") */
    uint8_t  version;       /* SLP_VERSION 1 */
    uint8_t  type;          /* enum slp_msg_type */
    uint16_t length;        /* longueur totale du datagramme, en-tête compris */
    uint16_t flags;         /* réservé (0) */
    uint32_t seq;           /* compteur par émetteur et par canal */
};
```
Règle d'extension : un récepteur **ignore** les types inconnus et les octets
au-delà de la structure qu'il connaît (les `reserved[]` sont prévus pour
ça) ; un changement incompatible incrémente `version` et l'ANNOUNCE porte
`proto_min`.

### D3 — Découverte par **broadcast HELLO**, session exclusive par **BIND**, vie par **PING**
```
VST                                   CIS
 |-- HELLO (broadcast 255.255.255.255:55150 sur chaque interface,
 |          + unicast vers l'IP manuelle si renseignée, toutes les 1 s) -->
 |<-- ANNOUNCE (unicast : identité + capacités + état bound) ------------|
 |-- BIND {session, stream_port, mode, hid_rate, dpi} (retry 300 ms ×10) -->
 |<-- BIND_ACK {status, LAYOUT négocié : px/ligne, fragments, hid_rate…} -|
 |-- PING (500 ms) --> / <-- PONG {uptime, lines/s, cal_state, temp…} ----|
 |-- UNBIND (best effort à la fermeture) ---------------------------------->
```
- Le **VST configure son parseur depuis BIND_ACK** (plus de constantes
  `UDP_MAX_NB_PACKET_PER_LINE` / `sensorDpi` en dur pour le réassemblage) :
  c'est ce qui rend un futur device (autre largeur, autre capteur) supportable
  sans casser le VST.
- **Cible du flux** : `stream_mode 0` = unicast vers **l'adresse source du
  BIND** (pas besoin que le VST connaisse sa propre IP), `1` = groupe
  multicast donné (plusieurs process récepteurs). Le CIS n'a plus à connaître
  l'IP du PC.
- **Session exclusive** : un BIND d'un autre pair est refusé `BUSY` tant que
  la session vit ; expiration après `session_timeout_ms` (3000) sans PING.
  L'ANNOUNCE expose `bound` + `bound_peer_ip` pour l'afficher dans le VST.
- **Repli non lié** (`stream_when_unbound`, défaut ON, réglable sur la page
  web) : sans session, le CIS continue d'émettre LINE+HID vers
  `network_dest_ip:network_udp_port` comme aujourd'hui (Wireshark, outils
  tiers). OFF = silence radio hors session.
- **mDNS** : le service `_apple-midi` disparaît ; on garde le hostname
  (`sp3ctra-XXXX.local` pour la page web) et un service `_sp3ctra._udp` sur
  55150 (TXT `uid=`, `fw=`) — **secondaire** : le VST n'en dépend pas.

### D4 — Identité par unité
Dérivées du UID 96 bits STM32 : **MAC administrée localement**
`02:53:33:xx:xx:xx` (hash du UID ; corrige la MAC unique pour toutes les
unités), **numéro de série** `S3-XXXXXXXX`, **nom** `Sp3ctra-XXXX`, hostname
mDNS. Nouveau `Common/Src/sys_identity.c`. `hw_revision` = constante de
build (`config.h`) en attendant un strap matériel.

### D5 — Nouveau paquet LINE : fragments de **432 px, 8 par ligne à 400 DPI (4 à 200 DPI)**
1320 o par datagramme (< 1472 o MTU), −33 % de datagrammes par ligne (c'est
le coût `netconn_send` sur le CM7 qui borne le débit de lignes). Envoi dans
l'ordre naturel. Champs explicites `pixel_offset` / `pixel_count` : plus
d'ambiguïté à 200 DPI. `scanline_CM4[8]` (8 × 1320 = 10 560 o, même taille
qu'aujourd'hui) — `gui_cis_display.c` adapte son indexation. Constante
`SLP_LINE_FRAGMENT_PIXELS` unique, `CIS_MAX_PIXELS_NB % 8 == 0` vérifié.

### D6 — Paquet HID **décorrélé du balayage**, à cadence fixe négociée
Nouvelle tâche CM7 `hidTask` (période 1 ms, comme l'ex-`midiTask`) :
échantillonne l'IMU à 1 kHz (ODR), émet un `slp_hid` toutes les
`1000/hid_rate_hz` ms (**200 Hz** par défaut) **et immédiatement sur tout
front de bouton**. Le paquet porte l'état des boutons (bitmask) **et un
compteur de fronts par bouton** : le VST détecte les fronts même en cas de
perte de datagramme (et resynthétise une paire press/release si la parité
l'exige). L'IMU est envoyée en **g** et **dps** (fin de l'ambiguïté m/s²),
plus température. `udpClient_sendPackets` n'envoie plus que l'image.

### D7 — Le MIDI est fabriqué **dans le VST**, injecté **avant** `midiMap_.processMidi`, dans un tampon séparé
Nouveau `midi/HidMidiMapper` (par `PluginProcessor`) : à chaque bloc, lit le
dernier instantané HID (seqlock lock-free publié par `udpThread`, curseur
par consommateur) et produit des messages MIDI dans un `juce::MidiBuffer`
**privé** `hidMidi_`, consommé par `midiMap_.processMidi(hidMidi_)` juste
après `processMidi(midiMessages)`. Conséquences :
- le **MIDI-learn existant** (clic droit sur n'importe quel contrôle) capture
  un bouton ou un axe du CIS exactement comme un contrôleur matériel ;
- les notes générées par un bouton **n'atteignent pas** les synthés (PITCH,
  MASK, LuxSynth, LuxWave lisent `midiMessages`, pas `hidMidi_`) ni le bus
  MIDI de sortie — pas de boucle, pas de note fantôme ;
- « Écho vers une sortie MIDI virtuelle `Sp3ctra HID` » (pour piloter d'autres
  plugins dans un DAW) = extension ultérieure via l'infra `MidiTapSink` (§10).

Contrôles et lois :
| Contrôle | Types | Réglages |
|---|---|---|
| SW1, SW2, SW3 | Off / **CC** (127 press, 0 release) / **Note** (on/off) / **CC toggle** (bascule 0↔127 à chaque press) | canal 1-16, numéro 0-127 |
| ACC X/Y/Z (g), GYRO X/Y/Z (dps), TILT pitch/roll (dérivés de ACC) | Off / **CC 7 bits** / CC 14 bits (MSB n, LSB n+32) | canal, numéro, **min / max** en unités physiques, **zone morte**, **lissage** (1 pôle, ms), bipolaire (centre = 64) |
Émission continue **sur changement de valeur quantifiée seulement**, avec un
plancher de **5 ms** entre deux messages d'un même contrôle (hystérésis
½ pas) — pas de flot inutile vers le moteur de mapping. Défauts : SW1-3 →
CC 20/21/22 canal 1 (les défauts historiques du firmware), IMU Off.

Paramètres APVTS (non automatisables, sauvés avec la session) :
`sp3ctraHid{Sw1,Sw2,Sw3,AccX,AccY,AccZ,GyrX,GyrY,GyrZ,TiltP,TiltR}{Type,Chan,Num}`
+ `{Min,Max,Dead,Smooth}` pour les continus (≈ 60 params).

### D8 — Bloc SP3CTRA : trois faces **PLAY | CONTROLS | SETUP**
Via le mode segments personnalisés de `FaceSwitchBar` (comme VIDEO SCROLL) ;
`blockHasSetup` reste vrai pour ce bloc, `layoutZone3` gagne le cas
`Chain1Source/Chain2Source` en face 1.
- **PLAY** : inchangé (transport, fade, acquisition).
- **CONTROLS** (nouveau `image/Sp3ctraControlsPage.h`, squelette
  `ModuleChrome`, contrôles lime `kColHandle`) : section **MIDI OUT** = une
  ligne par contrôle {vumètre live 30 Hz avec rémanence (règle des éditeurs),
  Type, Ch, Num, Min, Max…} ; section **FEEDBACK** = LED1-3 : mode
  {Off / **Press** (local, comme aujourd'hui) / **Follow** / **Manual**} ;
  OLED : mode {Off / **Chain** / All}, Hold (ms).
- **SETUP** : section LINK réécrite = **liste des devices découverts**
  (nom, série, FW, IP, état *libre / lié / utilisé par x.x.x.x*, bouton USE)
  + « IP manuelle » de repli + port/adresse du flux ; sections CIS / IMU /
  GUI / FIRMWARE conservées ; sections **MIDI CHANNELS** et **RTP-MIDI /
  ports MIDI** supprimées ; NETWORK garde IP/masque/passerelle du device +
  `stream_when_unbound`. Device préféré (UID) dans `MachinePrefs`
  (`sp3ctraPreferredDeviceUid`) : machine-scoped, jamais imposé par une
  session chargée d'ailleurs.

### D9 — Retour LEDs : `LED_SET` reprend le **modèle d'animation existant** du CM4
Par LED : `{brightness_1 (0-100), glide_1, time_1_ms, brightness_2, glide_2,
time_2_ms, blink_count, flags}` — exactement `leds_initCommand()`, donc
aucun nouveau moteur côté CM4 ; échelle **0..100** partout (fin du bug
×1000/127). `flags.bit0` = inhiber l'allumage forcé local pendant l'appui.
Modes VST :
- **Press** : rien n'est envoyé, comportement local actuel.
- **Follow** : la LED reflète la **valeur du paramètre sur lequel le bouton
  est appris** (recherche inverse dans `MidiMappingEngine` : slot dont
  (type, canal, numéro) = le MIDI produit par ce bouton → `paramId` →
  valeur normalisée ; toggle/2 états → on/off, continu → luminosité) ; envoi
  sur changement, ≤ 20 Hz, réémission toutes les 2 s (UDP sans accusé).
- **Manual** : `sp3ctraLed{1,2,3}Level` (0..1, **automatisable et
  MIDI-learnable**) → luminosité.

### D10 — Retour OLED : `OLED_OVERLAY` = **jusqu'à 3 items** {label, valeur, barre}, TTL côté device
```c
struct slp_overlay_item { char label[14]; char value[10]; uint16_t norm; /* 0..65535, 0xFFFF = pas de barre */
                          uint8_t flags; /* bit0 bipolaire (repère central), bit1 « dernier touché » */ uint8_t reserved; };
struct slp_oled_overlay { struct slp_hdr hdr; uint16_t ttl_ms; uint8_t count; uint8_t layout; /* 0 auto */
                          struct slp_overlay_item item[3]; };
```
- **VST** (`feedback/DeviceFeedback`, timer message-thread 30 ms du
  processeur) : `parameterChanged()` (toutes sources : UI, MIDI, automation)
  pousse `(index, tick)` dans un petit ring lock-free ; le timer garde les
  **3 derniers paramètres distincts** touchés dans la fenêtre Hold, filtre
  selon le mode — **Chain** = paramètre d'un module d'une chaîne qui héberge
  un IN SP3CTRA (résolution `navTargetForParam` → instance → chaîne), plus
  les globaux du module (transport) ; **All** = tout — puis émet
  `OLED_OVERLAY` coalescé (≤ 20 Hz, seulement si changement), label =
  `AudioProcessorParameter::getName(14)`, value = `getCurrentValueAsText()`,
  norm = `getValue()`.
- **CM4** (`gui_overlay.c`) : bandeau en haut du waterfall (le waterfall
  continue dessous) ; 1 item → police 16×16 label + valeur, barre pleine
  largeur (bande 26 px) ; 2-3 items → lignes 8×8 de 10 px {label · valeur ·
  barre} ; l'item « dernier touché » en contraste max, les autres à mi-niveau ;
  disparition à `ttl_ms` (défaut 1500 ms, renouvelé à chaque message) ;
  `OLED_CLEAR` immédiat. L'en-tête OLED affiche aussi un pictogramme **LINK
  ●** quand une session est liée (état lu dans `shared_feedback`).
- Mémoire partagée : nouveau `shared_feedback` (`globals.h`) = `{led_seq,
  led_mask, led[3], overlay_seq, overlay, link_state, peer_ip[4]}` écrit par
  CM7 (`SCB_CleanDCache_by_Addr` comme `packet_IMU`), scruté par CM4 sur
  changement de `*_seq` (même motif que `led_update_requested`).

### D11 — Configuration device **sur le lien** (CFG TLV), HTTP conservé pour le navigateur et l'upload firmware
`CFG_GET {ids}` / `CFG_SET {items}` / `CFG_REPLY {items, reboot_required}`,
item = `{uint16 id, uint8 type (u8/u16/u32/f32/ip4/str), uint8 len, valeur}`.
Ids : DPI, OVERSAMPLING, HAND, GYRO_FS, ACCEL_FS, GUI_SHOW_IMU, GUI_INVERT,
SCREENSAVER_S, MOTION_THR_ACC/GYRO, NET_IP/MASK/GW/DEST_IP, STREAM_PORT,
STREAM_WHEN_UNBOUND, MDNS_ENABLED, CTRL_PORT ; `CAL_START {CIS|IMU}`,
progression dans PONG. Supprime la dépendance du VST à `Sp3ctraDeviceClient`
(et la contention PCB HTTP ⇄ UDP des 150 ms). **Jalon V5**, après que le reste
tourne — le HTTP existant reste fonctionnel entre-temps.

### D12 — Versions et compatibilité
Le format du flux change → firmware **4.0.0**, VST **1.5.0** ; le VST 1.5
exige `proto ≥ 1` (ANNOUNCE) et affiche via `Sp3ctraDialog` un message
standard « firmware trop ancien, mettre à jour (page web / upload) » ; le
firmware 4.0 n'est plus lisible par le VST 1.4 ni par l'external Max
(`Deprecated/`). Pas de mode legacy double : un seul format.

### D13 — Outillage de test **sans l'autre extrémité**
- `Sp3ctra_CIS_Firmware/scripts/slp_tool.py` : `discover`, `bind`, `hid`
  (dump fronts/IMU), `led`, `overlay`, `cfg`, `stat` — valide le firmware
  sans le VST (à lancer par l'utilisateur : le shell de l'agent n'a pas la
  permission « Réseau local »).
- `Sp3ctra_VST/scripts/slp_fake_device.py` : device simulé (répond
  HELLO/BIND/PING, stream LINE synthétique + HID pilotable au clavier,
  affiche LED_SET/OVERLAY reçus) — valide le VST sur le Mac **sans
  matériel**, y compris via le skill `verify`.
- Un module `slp.py` (structs `struct.Struct`) commun aux deux, généré à la
  main depuis `sp3ctra_link.h` (les `static_assert` de taille sont la
  référence).

### D14 — Ce qui est retiré
Firmware : `rtpmidi_session.c`, `rtpmidi_packet.c`, `rtpmidi.h`,
`midi_button_mapper.*`, `midi_led_mapper.*`, `tcp_client.*`, `midiTask`,
mDNS `_apple-midi`, `packet_StartupInfo`/`packet_Button`/`packet_Leds`/
`packet_IMU` (remplacés), champs `rtpmidi_*`, `midi_button_*`,
`ui_button_delay`, `network_tcp_port` de `shared_config` + clés `CONFIG.TXT`
+ endpoints `get/setRtpMidiMode`, `get/setMidiButtonConfig`, champs RTP de
`updateNetworkConfig` + sections `config.html` ; `RTPMIDI_*` de `config.h`.
VST : sections RTP-MIDI / MIDI CHANNELS de `SourceSetupPanel`, champs
`rtpMidi*` / `MidiButton` de `Sp3ctraDeviceClient`, structs
`packet_*`/enums de `multithreading.h`, champs `imu_*` de `Context`.

## 4. Spécification SLP v1 (extraits de `sp3ctra_link.h`)

```c
#define SLP_MAGIC        0x5333u
#define SLP_VERSION      1u
#define SLP_CTRL_PORT    55150u
#define SLP_STREAM_PORT  55151u
#define SLP_LINE_FRAGMENT_PIXELS 432u
#define SLP_MAX_LEDS     4u
#define SLP_MAX_BUTTONS  4u

enum slp_msg_type {
    /* CONTROL, VST -> device */
    SLP_HELLO = 0x01, SLP_BIND = 0x02, SLP_UNBIND = 0x03, SLP_PING = 0x04,
    SLP_LED_SET = 0x10, SLP_OLED_OVERLAY = 0x11, SLP_OLED_CLEAR = 0x12,
    SLP_CFG_GET = 0x20, SLP_CFG_SET = 0x21, SLP_CAL_START = 0x22,
    /* CONTROL, device -> VST */
    SLP_ANNOUNCE = 0x81, SLP_BIND_ACK = 0x82, SLP_PONG = 0x84,
    SLP_CFG_REPLY = 0xA0, SLP_ERROR = 0xFF,
    /* STREAM, device -> VST */
    SLP_LINE = 0xC0, SLP_HID = 0xC1,
};

/* Capacités (ANNOUNCE.features / BIND.want_features) */
#define SLP_FEAT_LED_SET      (1u << 0)
#define SLP_FEAT_OLED_OVERLAY (1u << 1)
#define SLP_FEAT_CFG          (1u << 2)
#define SLP_FEAT_CAL          (1u << 3)
#define SLP_FEAT_HID_BUTTONS  (1u << 8)
#define SLP_FEAT_HID_ACC      (1u << 9)
#define SLP_FEAT_HID_GYRO     (1u << 10)
#define SLP_FEAT_HID_TEMP     (1u << 11)

struct slp_hello    { struct slp_hdr hdr; uint8_t vst_version[3]; uint8_t proto_min; uint32_t want_features; };

struct slp_announce {
    struct slp_hdr hdr;
    uint8_t  uid[12];  uint8_t mac[6];
    uint8_t  hw_family; /* 1 = CIS */  uint8_t hw_revision;
    uint8_t  fw_version[3]; uint8_t proto_min;
    char     name[16];                    /* "Sp3ctra-1A2B" */
    uint16_t ctrl_port; uint16_t stream_port;   /* défaut de flux du device */
    uint8_t  bound; uint8_t bound_peer_ip[4]; uint8_t reserved0[3];
    uint32_t features;
    uint8_t  n_buttons, n_leds, led_kind /* 0 mono PWM, 1 RGB */, imu_kind /* 0 none, 1 6 axes */;
    uint16_t display_w, display_h; uint8_t display_bpp, n_dpi;
    uint16_t dpi[4]; uint16_t pixels_at_dpi[4];
    uint16_t line_rate_max, hid_rate_max;
    uint8_t  reserved1[14];
};

struct slp_bind {
    struct slp_hdr hdr;
    uint32_t session;          /* nonce VST */
    uint16_t stream_port;
    uint8_t  stream_mode;      /* 0 unicast vers la source du BIND, 1 multicast */
    uint8_t  reserved0;
    uint8_t  mcast_group[4];
    uint16_t hid_rate_hz;      /* 0 = défaut device */
    uint16_t dpi;              /* 0 = conserver */
    uint32_t want_features;
    uint8_t  vst_version[3]; uint8_t reserved1[5];
};

struct slp_bind_ack {
    struct slp_hdr hdr;
    uint32_t session;
    uint8_t  status;           /* 0 OK, 1 BUSY, 2 UNSUPPORTED, 3 BAD_PARAM */
    uint8_t  reserved0[3];
    /* LAYOUT négocié — le VST configure son parseur depuis ces champs */
    uint16_t dpi; uint16_t pixels_per_line;
    uint8_t  fragment_count; uint8_t reserved1; uint16_t fragment_pixels;
    uint16_t line_packet_bytes; uint16_t hid_rate_hz; uint16_t hid_valid_mask;
    uint16_t session_timeout_ms;
    uint8_t  reserved2[8];
};

struct slp_ping { struct slp_hdr hdr; uint32_t session; uint32_t vst_time_ms; };
struct slp_pong { struct slp_hdr hdr; uint32_t session; uint32_t vst_time_ms; uint32_t uptime_ms;
                  uint32_t lines_sent; uint16_t line_rate_lps; uint8_t cal_state; uint8_t cal_progress;
                  int16_t temp_c_x10; uint8_t link_flags; uint8_t reserved[5]; };

struct slp_led_cmd { uint8_t brightness_1, glide_1; uint16_t time_1_ms;
                     uint8_t brightness_2, glide_2; uint16_t time_2_ms;
                     uint16_t blink_count; uint8_t flags; uint8_t reserved; };   /* 12 o */
struct slp_led_set { struct slp_hdr hdr; uint8_t led_mask; uint8_t reserved[3]; struct slp_led_cmd led[SLP_MAX_LEDS]; };

struct slp_line {                              /* 1320 o */
    struct slp_hdr hdr;                        /* seq = compteur de datagrammes */
    uint32_t line_id;
    uint16_t pixel_offset, pixel_count;        /* fragment : [offset, offset+count) */
    uint8_t  fragment_index, fragment_count;
    uint16_t line_period_us;                   /* 0 si inconnu */
    uint8_t  r[SLP_LINE_FRAGMENT_PIXELS], g[SLP_LINE_FRAGMENT_PIXELS], b[SLP_LINE_FRAGMENT_PIXELS];
};

struct slp_hid {                               /* 72 o */
    struct slp_hdr hdr;
    uint32_t timestamp_us;                     /* horloge monotone device */
    uint16_t valid_mask;                       /* SLP_FEAT_HID_* >> 8 */
    uint8_t  button_count; uint8_t button_state;   /* bit i = bouton i pressé */
    uint32_t button_seq[SLP_MAX_BUTTONS];      /* compteur de fronts par bouton */
    float    acc[3];                           /* g */
    float    gyro[3];                          /* dps */
    float    temp_c;
    uint8_t  reserved[8];
};
```
`slp_cfg_*` et `slp_cal_start` sont spécifiés au jalon V5 (D11).

## 5. Architecture firmware (cible)

| Fichier | Rôle |
|---|---|
| `Common/Inc/sp3ctra_link.h` | contrat de fil (D2) |
| `Common/Src/sys_identity.c` + `.h` | UID → MAC / série / nom (D4) ; utilisé par `ethernetif.c` (MAC), `lwip.c` (hostname), ANNOUNCE |
| `CM7/Application/Src/link_server.c` + `.h` | tâche `linkTask` (priorité Normal, 4096 o) : `netconn` UDP lié sur 55150, `netconn_recv` avec timeout 100 ms, dispatch HELLO/BIND/UNBIND/PING/LED_SET/OLED_*/CFG_*/CAL ; structure `link_session` ; réponses par `netconn_sendto` ; expiration sans PING ; écrit `shared_feedback` ; expose `link_getStreamTarget()` |
| `CM7/Application/Src/hid_task.c` + `.h` | tâche 1 ms : IMU 1 kHz, `slp_hid` à `hid_rate_hz` + sur front bouton ; propre `netconn` UDP vers la cible de flux (D6) |
| `CM7/Peripheral/Src/udp_client.c` | ne fait plus que LINE : en-tête `slp_line`, ordre naturel, `udpClient_setTarget(ip, port)` appelé par le serveur de lien (BIND / expiration / repli `network_dest_ip`) |
| `Common/Inc/globals.h` | `scanline_CM4[8]` de type `slp_line`, `shared_feedback`, retrait des `packet_*` |
| `CM4/Application/Src/gui_overlay.c` + `.h` | bandeau OLED (D10), appelé après `gui_displayImage()` dans `gui_core.c` |
| `CM4/Peripheral/Src/leds.c` | consomme `shared_feedback.led[]` (échelle 0..100, flag d'inhibition d'appui) |
| `CM4/Application/Src/gui_cis_display.c` | indexation 8 fragments × 432 px |
| `CM7/Application/Src/http_server.c`, `config.html`, `file_manager_config.c`, `config.h` | retrait RTP/MIDI (D14), ajout `link_ctrl_port`, `stream_when_unbound`, `GET /getDeviceInfo` (uid, série, fw, état de lien) |
| `CM7/LWIP/App/lwip.c`, `lwipopts.h` | mDNS : hostname unique + `_sp3ctra._udp` ; budget inchangé (`MEMP_NUM_NETCONN 16`, `MEMP_NUM_UDP_PCB 8` : +2 netconn UDP) |
| `CM7/Core/Src/freertos.c` | init : `sys_identity_init()` → … → `link_serverInit()` → `hid_taskInit()` ; suppression du bloc RTP-MIDI et de `midiTask` |

Contraintes RT respectées : aucune allocation ni `printf` dans `hidTask` ni
dans le chemin d'envoi ; `linkTask` peut logger (hors RT). Broadcast : lwIP
accepte les datagrammes broadcast sur un PCB lié à `IP_ADDR_ANY`
(`IP_SOF_BROADCAST` non défini → pas de filtrage). Le `.ioc` n'est **pas**
touché (pas de nouveau périphérique).

## 6. Architecture VST (cible)

| Fichier | Rôle |
|---|---|
| `communication/link/sp3ctra_link.h` | copie du contrat (D2) |
| `communication/link/Sp3ctraLink.{h,cpp}` | `juce::Thread` dans `Sp3ctraSharedCore` : `juce::DatagramSocket(true)` (broadcast), `IPAddress::getAllAddresses` + `getInterfaceBroadcastAddress` pour HELLO sur chaque interface ; machine d'état `Searching → Binding → Bound` ; registre de devices (UID → `DeviceInfo`, expiration 5 s) avec `ChangeBroadcaster` ; PING 500 ms ; file d'envoi coalescée (LED/OVERLAY : dernier état par cible) ; API `bindTo(uid)`, `unbind()`, `sendLed()`, `sendOverlay()`, `layout()` |
| `communication/link/hid_snapshot.h` (C) | instantané HID seqlock (`__atomic_*`, comme `internal_source.c`) publié par `udpThread`, lu sur le thread audio |
| `communication/network/udp.c`, `threading/multithreading.c` | tampon de réception `max(sizeof(slp_line), …)` ; validation `magic/version/length` ; `SLP_LINE` → réassemblage par `pixel_offset/pixel_count` avec `fragment_count` **du layout négocié** ; `SLP_HID` → snapshot ; compteur de datagrammes perdus (`hdr.seq`) exposé au SETUP |
| `midi/HidMidiMapper.{h,cpp}` | D7 ; appelé dans `processBlock` juste après `midiMap_.processMidi(midiMessages)` |
| `feedback/DeviceFeedback.{h,cpp}` | D9 + D10 (timer 30 ms du processeur, ring de touches alimenté par `parameterChanged`) |
| `image/Sp3ctraControlsPage.h` | face CONTROLS (D8), `ModuleChrome`, `Sp3ctraBarSlider`, `MidiLearnAttachment` sur les `sp3ctraLedNLevel` |
| `ui/setup/SourceSetupPanel.{h,cpp}` | LINK = liste des devices + IP manuelle ; retrait RTP/MIDI ; affichage FW / série / pertes |
| `PluginEditor.{h,cpp}` | segments `PLAY | CONTROLS | SETUP` pour `Chain1Source/Chain2Source`, `layoutZone3`, `applyZone3Visibility`, persistance `selSourceFace` |
| `PluginProcessor.{h,cpp}` | params D7/D9/D10, `navTargetForParam` pour les nouveaux ids, retrait des lectures `sensorDpi` du parseur (le layout vient de BIND_ACK ; `sensorDpi` reste la **demande** envoyée dans BIND) |
| `ui/ChainRackComponent.cpp` | LED du bloc SP3CTRA : Off = stop, Idle = pas de session, Active = liée et lignes qui avancent |
| `session/MachinePrefs.h` | `sp3ctraPreferredDeviceUid`, `sp3ctraManualIp` |
| `CMakeLists.txt`, `scripts/check_link_header.sh`, `scripts/slp_fake_device.py` | sources, garde du contrat, device simulé |

## 7. Jalons

### V0 — Contrat et outillage (½ j)
`sp3ctra_link.h` (+ `static_assert`), `slp.py`, `slp_tool.py`,
`slp_fake_device.py`, `check_link_header.sh`. Commit du portage CMSIS v2
en attente dans le repo firmware **avant** de commencer.

### V1 — Firmware : lien + nouveau flux, retrait RTP-MIDI (2-3 j)
D3, D4, D5, D6, D14 côté firmware ; `build.sh` vert (CM4 + CM7) ; flash par
l'utilisateur ; validation avec `slp_tool.py` (discover → bind → hid →
stat), Wireshark sur 55150/55151. FW 4.0.0.

### V2 — VST : client de lien + parseur v2 + SETUP (2 j)
`Sp3ctraLink`, parseur LINE/HID, snapshot HID, liste de devices, rack.
Validation avec `slp_fake_device.py` puis le vrai CIS (image identique à
aujourd'hui, pertes = 0 à 400 DPI). VST 1.5.0.

### V3 — VST : HID → MIDI + face CONTROLS (1-2 j)
D7, D8. Validation : MIDI-learn d'un bouton sur PLAY/STOP, d'ACC X sur un
paramètre de LEVELS ; cadence/latence mesurées (< 10 ms bouton → paramètre).

### V4 — Retour LEDs + OLED (2 j)
Firmware `gui_overlay.c` / `leds.c` / `shared_feedback` + VST
`DeviceFeedback`. Validation : tourner un slider dans le VST → bandeau OLED
< 100 ms, disparition à Hold ; mode Follow sur un toggle.

### V5 — CFG sur le lien, retrait du client HTTP dans le VST (1-2 j)
D11 ; `Sp3ctraDeviceClient` réduit à l'upload firmware ; HTTP inchangé pour
le navigateur.

### V6 — Nettoyage, docs, versions (½ j)
README firmware (protocole, ports, page web), `docs/` VST, CHANGELOG,
mémoire projet.

## 8. Plan de vérification
1. **Contrat** : `check_link_header.sh` ; `static_assert(sizeof(struct slp_line) == 1320)` etc. compilent dans les deux repos ; `slp.py` a les mêmes tailles.
2. **Firmware seul** (utilisateur, `slp_tool.py`) : ANNOUNCE reçu sur broadcast ; BIND_ACK avec layout 3456/8/432 (400 DPI) puis 1728/4/432 (200 DPI) ; HID à 200 ± 2 Hz, un paquet supplémentaire par front, `button_seq` monotone ; expiration de session 3 s sans PING → repli `network_dest_ip` (Wireshark) ; second `bind` depuis une autre machine → BUSY ; `led` et `overlay` visibles sur le matériel ; `stats_display` lwIP sans « MEMP empty » après 10 min.
3. **VST seul** (`slp_fake_device.py`, skill `verify`) : device dans la liste SETUP en < 2 s, session liée, image synthétique dans la vue CIS ; CONTROLS : le vumètre suit le HID simulé, MIDI-learn capture le CC produit ; LED_SET/OVERLAY reçus par le simulateur avec les bons contenus.
4. **Bout en bout** (utilisateur) : image identique au 1.4 (même DPI, même débit `getFreq`), zéro perte 10 min à 400 DPI ; bouton → paramètre ; slider → OLED ; mode Follow ; débranchement Ethernet → rack Idle en < 3 s, reprise automatique au rebranchement (le firmware reboote déjà sur perte de lien) ; standalone + VST3 dans un DAW simultanément → le second voit « utilisé par … » et peut suivre le flux en multicast.
5. **Non-régression** : sessions 1.4 se chargent (nouveaux params à défaut, `MachinePrefs` inchangés) ; Windows : `DatagramSocket` broadcast + `getInterfaceBroadcastAddress` (JUCE) ; pas de `clock_gettime` dans le nouveau C.

## 9. Risques et points de vigilance
- **Budget lwIP CM7** : +2 `netconn` UDP (lien, HID) et +200 `netconn_send`/s ; surveiller `MEMP_NUM_NETBUF` / mbox (`DEFAULT_UDP_RECVMBOX_SIZE 6`) — ne rien loguer dans `hidTask`. Le portage CMSIS v2 a montré la sensibilité aux priorités : `linkTask` **Normal**, jamais au-dessus de `tcpip`.
- **Cohérence cache CM7 → CM4** pour `shared_feedback` (région cacheable) : `SCB_CleanDCache_by_Addr` après écriture, `*_seq` écrit **en dernier** ; CM4 lit `seq` puis copie.
- **Pare-feu macOS / Windows** : le broadcast sortant passe, l'ANNOUNCE entrant arrive sur la socket qui a émis (même port) — comme le flux 55151 aujourd'hui ; documenter l'autorisation « réseau local » pour le standalone.
- **Plusieurs interfaces** (Wi-Fi + Ethernet) : HELLO sur toutes ; la réponse arrive sur celle du CIS ; BIND unicast implicite = adresse source vue par le CIS (bonne interface par construction).
- **Deux process VST** : session exclusive ; le second reçoit BUSY — mode multicast pour partager le flux, contrôle/feedback réservés au premier (dernier écrivain sinon). À documenter dans SETUP.
- **Débit MIDI interne** : 8 axes à 200 Hz → hystérésis + plancher 5 ms ; `MidiMappingEngine::processMidi` est O(slots × événements) (128 × ~40/bloc au pire) — mesurer avec `rt_profiler`.
- **Filtrage « Chain » de l'overlay** : nécessite la résolution paramètre → chaîne pour tous les ids (banqués, virtuels `smp:`/`eqh:`) — réutiliser `navTargetForParam` ; les ids non résolus passent en mode All seulement.
- **OLED 16 niveaux, 256×64** : 14 caractères 8×8 = 112 px pour le label ; valeurs tronquées à 10 ; police 16×16 pour 1 item = 12 caractères max → `getName(12)`.
- **Repo firmware** : 10 fichiers modifiés non commités ; tout commit de ce chantier doit partir d'un état propre (Conventional Commits, anglais).
- **Legacy** : l'external Max/PD et le Viewer (dépôts `CISYNTH_*`, `Deprecated/`) ne liront plus le flux 4.0 — assumé (D12).

## 10. Hors périmètre / suites possibles
- Sortie MIDI virtuelle « Sp3ctra HID » (écho des contrôles vers le DAW / d'autres plugins) via `MidiTapSink`.
- Menus OLED pilotés par les boutons (aujourd'hui inexistants) ; pages de « scènes » de mapping.
- Quaternion / orientation fusionnée côté firmware (`imu_kind 2`), LEDs RGB (`led_kind 1`) — déjà prévus dans ANNOUNCE.
- DHCP / AutoIP sur le CIS (la découverte broadcast fonctionne en statique et lèverait le prérequis « même sous-réseau » seulement avec DHCP des deux côtés).
- Mise à jour firmware sur le lien (rester en HTTP multipart).
- Chiffrement / authentification : réseau local point à point, non traité.

## 11. Points à trancher avant V0 (recommandation en gras)
1. Ports : **55150 contrôle / 55151 flux** (inchangé pour le flux) — ou autre plage ?
2. Fragments LINE : **432 px × 8** (D5) — ou conserver 288 × 12 pour minimiser les changements CM4 ?
3. Repli sans session `stream_when_unbound` : **ON par défaut** (compat outils tiers) — ou OFF ?
4. mDNS : **garder hostname + `_sp3ctra._udp`** — ou retirer mDNS entièrement (moins de timeouts lwIP) ?
5. Cadence HID par défaut : **200 Hz** (négociable dans BIND, max 1000).
6. Faces du bloc SP3CTRA : **PLAY | CONTROLS | SETUP** — ou fondre CONTROLS dans PLAY (page longue) ?
7. Contrôles continus : **ACC X/Y/Z, GYRO X/Y/Z + TILT pitch/roll dérivés** — ou seulement les 6 axes bruts ?
8. Filtre OLED : **mode Chain par défaut** (paramètres des chaînes hébergeant un IN SP3CTRA + transport), All en option.
9. Config device sur le lien (D11) : **oui, en V5** — ou rester en HTTP définitivement ?
10. Ordre : **V1 firmware d'abord** (validable avec `slp_tool.py`), puis V2 VST — ou les deux en parallèle via le device simulé ?

## 12. Complément du 2026-08-30 — décisions actées et état V0/V1

### 12.1 Réponses aux points de §11
| # | Décision |
|---|---|
| 1 | **Actée** : contrôle 55150, flux 55151. |
| 2 | **Revenue à 288 px × 12 fragments** (D5 amendée). Le capteur est lu par **3 voies ADC** (`CIS_ADC_OUT_LANES`), chacune couvrant un tiers des pixels : un fragment doit diviser 1152 px (400 DPI) **et** 576 px (200 DPI) ; sous la MTU (≤ 480 px) le seul diviseur commun est 288. 432 aurait cassé le remplissage par voie de `cis_imageProcess()`. Sans conséquence côté VST : le layout est **négocié** dans `BIND_ACK` (`fragment_pixels`, `fragment_count`, `pixel_offset`/`pixel_count` dans chaque datagramme), rien n'est codé en dur. Fonctionne à 200 DPI (6 fragments). |
| 3 | **Actée** : `stream_when_unbound` ON par défaut (page web + `CFG_STREAM_WHEN_UNBOUND`). |
| 4 | **mDNS retiré entièrement** (`LWIP_MDNS_RESPONDER 0`, plus de hostname ni de service ; le paramètre CubeMX `LWIP_MDNS` reste à 1 dans le `.ioc`, sans effet — à passer à 0 à la prochaine régénération). |
| 5 | **Actée** : HID 200 Hz par défaut, négociable 1–1000 Hz. |
| 6 | Détail UI en §12.3. |
| 7 | **Actée** : ACC X/Y/Z, GYRO X/Y/Z + TILT pitch/roll dérivés (11 contrôles). |
| 8 | Détail en §12.4. |
| 9 | **Actée** : configuration sur le lien (`CFG_*`, déjà implémentée côté device en V1) ; la **mise à jour du firmware reste en HTTP** (`POST /upload` multipart, `Sp3ctraDeviceClient::uploadFirmware` conservé, section FIRMWARE du SETUP inchangée). |
| 10 | **Firmware d'abord** : V0 + V1 faits (ci-dessous), V2–V4 VST ensuite. |

### 12.2 État — V0 et V1 réalisés et **validés sur le CIS réel** (Sp3ctra-77DD, fw 4.0.0)
Dépôt firmware : commit du portage CMSIS v2 sur `master`, branche **`develop`** créée, tout le chantier dessus.
- **Contrat** : `Common/Inc/sp3ctra_link.h` (packed, `_Static_assert` sur chaque taille) ; miroir Python `scripts/slp/slp.py`.
- **Identité** : `Common/Src/sys_identity.c` (UID → MAC `02:53:33:xx:xx:xx`, `Sp3ctra-XXXX`, `S3-XXXXXXXX`) ; MAC appliquée dans `ethernetif.c` (+ patch dans `post_cubemx_restore.sh`) ; nom affiché sur l'écran de démarrage.
- **Serveur de lien** `CM7/Application/Src/link_server.c` : HELLO/ANNOUNCE, BIND/BIND_ACK (session exclusive, layout négocié, cible de flux = source du BIND ou multicast), PING/PONG (uptime, lignes envoyées, lps, état/progression de calibration, température), UNBIND, expiration 3 s, LED_SET (→ moteur d'animation CM4 existant, échelle 0..100, flag « pas d'allumage local »), OLED_OVERLAY/CLEAR (→ `shared_feedback`), **CFG_GET/SET** (18 ids, reboot différé pour DPI/réseau/port de lien, `file_writeConfig`), **CAL_START** (CIS non bloquant, IMU bloquant 1,2 s).
- **Tâche HID** `hid_task.c` : IMU échantillonnée à 1 kHz (plus par ligne), `slp_hid` à la cadence négociée + immédiat sur front, compteurs de fronts CM4, acc en **g**, gyro en dps, température ; publie `shared_imu` au CM4.
- **Flux LINE** : `udp_client.c` réécrit (en-tête `slp_line_hdr`, ordre naturel, `line_period_us`, cible pilotée par le lien, repli statique), `cis.c`/`cis_scan.c` sur `struct slp_line_cis`.
- **CM4** : `gui_overlay.c` (1 item = bande 26 px police 16 px + barre pleine largeur ; 2–3 items = lignes 10 px label · valeur · barre 54 px ; bipolaire = repère central ; surligné = contraste max ; TTL ; bandeau « VST LINKED / VST LOST » 1,5 s sur changement d'état), `leds.c` (inhibition de l'allumage local par LED), `gui_imu.c` sur `shared_imu` (seuils de mouvement enfin en g), résistant au contenu aléatoire de la RAM partagée NOLOAD (détection de changement de `*_seq` uniquement).
- **Retraits** : `rtpmidi_*`, `midi_button_mapper`, `midi_led_mapper`, `tcp_client`, `midiTask`, mDNS, champs `rtpmidi_*`/`midi_button_*`/`mdns_enabled`/`network_tcp_port`/`ui_button_delay` de `shared_config`, clés `CONFIG.TXT`, endpoints et sections web. Ajouts web : bloc DEVICE (nom, série, MAC, état du lien, rafraîchi 1 s), Link Port, Stream w/o host, `GET /getDeviceInfo`.
- **Outillage** : `scripts/slp/slp_tool.py` (discover/stat/hid/lines/led/overlay/clear/cfg/cal), `scripts/slp/slp_fake_device.py` (device simulé pour le VST — **tout l'outillage Python vit dans le dépôt firmware**, un seul `slp.py`), `scripts/sync_subdir_mk.py` (resynchronise les `subdir.mk` + `objects.list` générés par CubeIDE après ajout/suppression de sources — indispensable pour `build.sh`).
- Versions : firmware **4.0.0** (`FW_VERSION_MAJOR/MINOR/PATCH` numériques pour l'ANNOUNCE).

**Validation matériel du 2026-08-30** (outil `slp_tool.py`, log UART) : découverte par broadcast dirigé,
BIND/PING/PONG (rtt 1 ms), **HID 200 Hz et ~995 lignes/s sans perte**, LED_SET/OLED_OVERLAY acceptés,
CFG_GET (18 ids) / CFG_SET avec écriture flash et rejet hors plage, CAL_START IMU (Z = -1,007 g après
calibration, X/Y ≈ 0). Quatre bugs trouvés et corrigés grâce au matériel : pile `linkTask` 4 Ko → 16 Ko
(le `FIL` FatFs embarque un secteur de 4 Ko → `Error_Handler` au premier CFG_SET) ; réponses copiées
dans un pbuf lwIP (`netbuf_ref` sur un buffer statique n'est pas sûr avec le DMA ETH) ; calibration
accéléro en unités mixtes (biais ×9,81 → +7,4 g au repos, bug préexistant) ; échantillonnage IMU figé
après calibration (attente SPI idle, abort avant réinit, auto-réparation dans `TIM_Callback`, tampon DMA
aligné sur sa ligne de cache) ; **le CM4 ne peut pas lire l'UID du MCU** (bus fault à 0x1FF1E800 →
HardFault dès la première image de boot : plus d'animation, écran figé, LEDs/OLED muets — le CM7 publie
désormais `shared_feedback.device_name` avant de libérer le CM4, `sys_identity.c` est CM7-only ; diagnostiqué
par lecture SWD hotplug des registres de faute du CM4 via `ap=3`). Outil : `--via-broadcast a.b.c.255` (shell sans permission « Réseau
local »), `--stream-port`, filtrage par adresse source, lps mesuré côté CM7, overlay/lien réveillent
l'écran de veille. Restent à voir de visu : LED2, bandeau OLED, réveil de veille.

**Écran de boot et veille (2026-08-30, demande utilisateur)** : logo réduit 180×46 centré sur l'animation
d'ondes + bandeau opaque (nom · version / IP · **étape de boot du CM7** avec points animés :
STARTING → CONFIG → NETWORK → LINK → IMU → SENSOR → READY, publiée dans `shared_feedback.boot_stage`) ;
veille = **fond seul, aucun logo ni texte** (toute forme tenue en place brûle l'OLED) : l'animation d'ondes
**d'origine** est conservée telle quelle (modulation de fréquence/épaisseur par ligne) — une réécriture en
rubans dérivants a été essayée puis abandonnée (« effet Windows 95 ») ; seule protection ajoutée : un
**décalage vertical de tout le champ** d'un espacement de ligne toutes les 60 s (0,27 px/s, invisible d'une
image à l'autre, rebouclé sur l'espacement → image statistiquement identique mais aucune rangée à
éclairement moyen constant), plus une ligne rendue au-delà de chaque bord et `sin()` → `sinf()` ; puis **extinction du panneau** (`0xAE`) après `DEFAULT_SCREENSAVER_DISPLAY_OFF_SEC` = 600 s, rallumage sur
mouvement / bouton / overlay / lien. Candidat CFG ultérieur : délai d'extinction configurable.

Ancienne consigne (pour mémoire) : flasher (`scripts/flash.sh all`), puis depuis un Terminal
`python3 scripts/slp/slp_tool.py discover` → `stat` → `hid` (appuyer sur les boutons) → `lines` → `led` → `overlay` → `cfg get`.
Les fichiers `Release/` sont ignorés par git : `sync_subdir_mk.py` a été appliqué localement ; CubeIDE régénère les mêmes listes à la prochaine ouverture.

### 12.3 Point 6 — la face CONTROLS du bloc SP3CTRA, en détail
Trois faces **`PLAY | CONTROLS | SETUP`** (mode segments personnalisés de `FaceSwitchBar`, persistées dans `selSourceFace`). PLAY ne change pas.

**CONTROLS** = une page `ModuleChrome` (cadre + titre, couleur de catégorie SRC pour l'affichage, lime `kColHandle` pour tout ce qui se touche), deux sections :

```
 MIDI OUT — le CIS vu comme un contrôleur
  contrôle   live            type        ch    num     min       max      ±
  SW1        [██████    ]    [CC     ▾]  [ 1]  [ 20]    –         –
  SW2        [          ]    [Note   ▾]  [ 1]  [ 21]    –         –
  SW3        [          ]    [Toggle ▾]  [ 1]  [ 22]    –         –
  ACC X      [====|     ]    [CC     ▾]  [ 1]  [ 30]  [-1.00 g] [+1.00 g]  [x]
  ACC Y      [   Off    ]    [Off    ▾]
  ACC Z      [   Off    ]    [Off    ▾]
  GYRO X     [==|       ]    [CC 14b ▾]  [ 2]  [ 16]  [-250 dps][+250 dps] [x]
  GYRO Y / GYRO Z   idem
  TILT P     [    |==   ]    [CC     ▾]  [ 1]  [ 40]  [-45 °]   [+45 °]    [x]
  TILT R     idem
  Deadzone [ 2 % ]      Smoothing [ 30 ms ]        (communs aux contrôles continus)

 FEEDBACK — ce que le VST renvoie au CIS
  LED1 [Follow ▾]   LED2 [Press ▾]   LED3 [Manual ▾]  level [======    ]
  OLED [Chain  ▾]   Hold [ 1500 ms ]
```
- **Une ligne par contrôle** (11) : `live` = barre de lecture seule à 30 Hz avec rémanence (règle des éditeurs de modules), pour les boutons pleine quand pressé ; `type` : boutons {Off, CC (127 à l'appui / 0 au relâché), Note (on/off), Toggle (bascule 0↔127 à chaque appui)}, continus {Off, CC 7 bits, CC 14 bits (MSB n, LSB n+32)} ; `ch` 1–16 ; `num` 0–127 ; `min`/`max` en unités physiques (g, dps, °) → 0..127 ; `±` = bipolaire (centre = 64, deadzone autour du centre). Tous ces réglages sont des `Sp3ctraBarSlider`/`ComboBox` standard (double-clic min/centre/max, molette inerte).
- **Pastille de mapping** à gauche du nom : allumée quand le MIDI produit par cette ligne est actuellement appris sur un paramètre (recherche inverse dans `MidiMappingEngine` sur (type, canal, numéro)), infobulle = nom du paramètre cible. Deux lignes qui produisent le même (canal, numéro) sont signalées en ambre (autorisé, mais visible).
- **Aucun bouton « learn » sur cette page** : l'apprentissage se fait comme pour n'importe quel contrôleur — clic droit sur le contrôle de destination (n'importe où dans le VST) → « MIDI Learn » → appuyer sur SW1 / incliner le CIS. Le MIDI passe par `midiMap_.processMidi(hidMidi_)` juste après le MIDI de l'hôte.
- Les lignes Off ne coûtent rien ; les lignes continues n'émettent que sur changement de valeur quantifiée (hystérésis ½ pas, plancher 5 ms) → au pire ~200 messages/s pour 8 axes très agités.
- Défauts : SW1–3 → CC 20/21/22 canal 1 (les défauts historiques du firmware) ; tout le reste Off.
- Hauteur ≈ 13 rangées de boîtes (`kBoxRowH` 30 px) + 2 légendes ≈ 470 px, dans le viewport défilant de la zone 3.

**SETUP** (section LINK réécrite, le reste inchangé sauf retraits) :
```
 LINK
  Devices    ● Sp3ctra-1A2B   192.168.100.1   fw 4.0.0   free                 [USE]
             ○ Sp3ctra-7F03   192.168.100.7   fw 4.0.0   used by 192.168.100.22
  Status     bound · 412 lps · HID 200 Hz · 0 lost · rtt 1 ms · 31.5 °C
  Manual IP  [192].[168].[100].[  1]  [ADD]        (repli quand le broadcast ne passe pas)
  Stream     port [55151]   mode [Unicast ▾ | Multicast 239.100.100.100]
  [x] Reconnect automatically to this device (machine)
```
- La liste vient de `Sp3ctraLink` (HELLO broadcast 1 s sur chaque interface + unicast vers les IP manuelles ; entrées expirées après 5 s). **USE** = `bindTo(uid)` ; un seul device lié ; un device « used by » reste visible mais son USE est grisé. Firmware < 4.0 ou `proto_min` > `SLP_VERSION` → ligne grisée + `Sp3ctraDialog` standard (« mettre à jour le firmware »).
- « Reconnect automatically » écrit `sp3ctraPreferredDeviceUid` dans `MachinePrefs` (machine, jamais dans la session).
- Sections retirées : MIDI CHANNELS, mDNS, RTP-MIDI, ports MIDI. Sections CIS / IMU / GUI passent sur `CFG_*` (V5) ; FIRMWARE reste en HTTP.
- Bloc du rack : LED **Off** = STOP, **Idle** = pas de session ou flux arrêté, **Active** = liée et lignes qui avancent.

### 12.4 Point 8 — quels paramètres s'affichent sur l'OLED (mode Chain)
Principe : l'OLED montre **ce que l'on est en train de toucher, si cela concerne le son produit par le CIS**.

- **Touché** = tout `parameterChanged()` quelle qu'en soit la source (souris, MIDI appris — y compris un bouton/axe du CIS —, automation de l'hôte). Chaque événement pousse (id, horodatage) dans un petit ring lock-free ; le timer 30 ms du processeur garde les **3 derniers paramètres distincts** touchés dans la fenêtre **Hold** (1500 ms par défaut, réglage sur CONTROLS), le plus récent surligné, un seul paramètre → grand affichage 16 px.
- **Filtre Chain** (défaut) — un paramètre passe s'il appartient à :
  1. un **module d'une chaîne qui contient un IN SP3CTRA** (chaîne 1 et/ou 2 : modules FX insérés, PITCH, MASK, LEVELS, EQ, etc.) ;
  2. le **module SP3CTRA lui-même** (transport PLAY/HOLD/STOP, Fade-In, vitesse d'acquisition) ;
  3. une **propriété de cette chaîne** (fond/pôle, sortie de chaîne) ;
  4. le **synthé de sortie de cette chaîne** (banque OUT/send LuxStral/LuxSynth/LuxWave/LuxGrain liée à cette chaîne, et l'engine correspondant).
  Sont **exclus** : les paramètres des chaînes sans IN SP3CTRA (ex. une chaîne SAMPLER ou SCORE seule), le mixage global (masters AUDIO MIX), les sorties vidéo (VIDEO SCROLL / VIDEO MIX), MIDI TAP, tout ce qui est session/UI (ports, IP, agencement).
  Résolution de l'appartenance : `navTargetForParam(id)` → bloc + instance/slot → `ChainModel` → indice de chaîne ; table mise en cache par id, reconstruite sur `onModelChanged` ; les ids non résolus ne s'affichent qu'en mode All.
- **Filtre All** : tout paramètre APVTS hors session/UI. **Off** : rien (le bandeau « VST LINKED/LOST » reste).
- **Contenu** : label = `getName(14)` (8 px) ou `getName(12)` (16 px), valeur = `getCurrentValueAsText()` tronquée à 10 (unités comprises : « -12.5 dB », « 1200 Hz », « Hold »), barre = valeur normalisée (drapeau bipolaire = repère central quand min < 0 < max ; pas de barre pour les booléens), TTL = Hold.
- **Cadence** : émission coalescée ≤ 20 Hz et seulement si le contenu change ; latence typique geste → OLED < 50 ms (ring → timer 30 ms → UDP). L'automation d'hôte peut faire défiler des dizaines de paramètres : le tri « 3 plus récents » et le coalescing bornent le débit à 20 datagrammes/s de 100 o.
- Seul le process **lié** émet ; un VST en mode multicast (second process) ne pilote ni l'OLED ni les LEDs.

### 12.5 V2 réalisé (VST, 2026-08-30) — client de lien, parseur v2, SETUP
- `communication/link/sp3ctra_link.h` = copie du contrat (garde `scripts/check_link_header.sh`, `--sync` pour recopier).
- `communication/link/Sp3ctraLink.{h,cpp}` : thread du canal de contrôle dans `Sp3ctraSharedCore` — HELLO sur le
  broadcast dirigé de chaque interface + hôtes manuels (= « Device IP »), registre des devices (expiration 5 s),
  politique de liaison (UID préféré `MachinePrefs link.preferredUid`, sinon l'unique device libre si
  `link.autoBind`), BIND avec **notre** port d'écoute (multicast si l'adresse configurée est multicast),
  PING 500 ms, expiration, re-BIND sur changement de port (restart UDP), file de retour coalescée
  (`setLed` / `setOverlay` / `clearOverlay` / `requestCalibration`, flushée par le thread),
  `ChangeBroadcaster` + `Status.generation` pour l'UI. Variable `SP3CTRA_LINK_BROADCAST_CTRL=1` = mode de
  test qui envoie le contrôle sur le broadcast dirigé (shells sans permission « Réseau local »).
- `communication/link/slp_rx_state.{h,c}` : instantané HID seqlock (`slp_hid_read` → génération) + statistiques
  de réception (`slp_rx_stats_snapshot`).
- `threading/multithreading.c` : parseur SLP v1 (magic/version/longueur), LINE réassemblée par
  `pixel_offset/pixel_count/fragment_count` (auto-descriptif, clip à la largeur allouée), HID → snapshot +
  miroir `Context`, comptage des pertes par flux, détection d'un firmware < 4.0, stats loguées toutes les 10 s ;
  structs `packet_*` supprimées.
- `PluginProcessor::pollLink()` (timer 30 ms) : pousse la politique machine et l'hôte manuel, puis à chaque
  changement de session **réconcilie `sensorDpi`** depuis `BIND_ACK` et recopie l'IP du device dans
  `deviceIpByte1..4` (hôte HTTP) — plus de saisie d'IP nécessaire.
- SETUP / LINK : liste des devices (nom · IP · fw · libre / lié / utilisé par… / non supporté, bouton USE/RELEASE),
  ligne d'état (DPI, lps, HID, rtt, température, pertes), case « connexion automatique », Device IP (cible HELLO
  + HTTP), Stream Port, Multicast ; sections MIDI CHANNELS et mDNS/RTP-MIDI retirées ; NETWORK gagne Link Port +
  Stream w/o host (`updateNetworkConfig` v4.0). `Sp3ctraDeviceClient` purgé des champs RTP/MIDI.
- **Validé sur le vrai CIS** : découverte, BIND (400 DPI, 12 × 288, HID 200 Hz), session confirmée côté device
  (`host 1.4.x`), lignes et HID reçus (stats), pertes 0.

### 12.6 V3 réalisé (VST, 2026-08-30) — le CIS comme contrôleur MIDI, face CONTROLS
- `midi/HidMidiMapper.{h,cpp}` : 11 contrôles (SW1-3, ACC X/Y/Z, GYRO X/Y/Z, TILT P/R dérivés de l'accéléro,
  ±90°), paramètres `sp3ctraHid<Ctrl>{Type,Chan,Num}` (+ `{Min,Max,Bipolar}` pour les continus), globaux
  `sp3ctraHidDeadzone` (%) et `sp3ctraHidSmoothMs`. Boutons : Off / CC (127-0) / Note (on-off) / Toggle ;
  continus : Off / CC / CC 14 bits (MSB n, LSB n+32). Fronts reconstruits depuis les compteurs `button_seq`
  (pertes tolérées), hystérésis ½ pas, lissage 1 pôle calé sur l'horodatage HID. Défauts : SW1-3 → CC 20/21/22
  canal 1, IMU Off.
- `processBlock` : `hidMidi_` (MidiBuffer privé, pré-dimensionné) rempli par le mapper puis consommé par
  `midiMap_.processMidi()` juste après le MIDI hôte — MIDI-learn natif, aucune fuite vers les synthés ni le bus
  MIDI de sortie. `MidiMappingEngine::paramForEvent()` = recherche inverse pour les pastilles de la page.
- `image/Sp3ctraControlsPage.h` : squelette `ModuleChrome`, une ligne par contrôle (vumètre live 30 Hz avec
  rémanence, Type, Ch, Num, Min, Max, ±), rangée Deadzone / Smoothing, section FEEDBACK (LED1-3 mode
  Off/Press/Follow/Manual + niveau Manual automatisable et MIDI-learnable, OLED Off/Chain/All + Hold) ; pastille
  « ● » + infobulle « Drives: <param> » quand la ligne est apprise quelque part.
- Bloc SP3CTRA en **PLAY | CONTROLS | SETUP** (`sourceFace_`, persisté `selSourceFace`) ; MIDI-follow d'un param
  `sp3ctraHid*/Led*/Oled*` → face CONTROLS (`ParamNavTarget::controlsFace`).
- Paramètres de retour créés (`sp3ctraLed{1,2,3}Mode/Level`, `sp3ctraOledMode`, `sp3ctraOledHoldMs`) ; la
  logique de retour (LED_SET / OLED_OVERLAY) est le jalon V4.
- Build vert, app stable ; validation interactive (appuis, inclinaison, learn) à faire par l'utilisateur.

### 12.7 V4 réalisé (2026-08-30) — retour LEDs + overlay OLED
- `feedback/DeviceFeedback.{h,cpp}` (timer 30 ms du processeur) :
  - **OLED** — chaque `audioProcessorParameterChanged` (souris, MIDI appris **y compris depuis le CIS**,
    automation hôte) horodate le paramètre ; le tick garde les **3 derniers distincts** dans la fenêtre Hold,
    filtre (**Chain** = module SP3CTRA + transport `image*`/`acqGate*`/`rawFreeze*` + tout module d'une chaîne
    hébergeant un IN SP3CTRA via `navTargetForParam` + `instanceChainHostsSp3ctra`, cache 1 s ; **All** = tout
    sauf la plomberie session/UI), compose label `getName(12|14)` · `getCurrentValueAsText()` · barre normalisée
    (bipolaire si la plage traverse 0, pas de barre pour un booléen) et envoie `OLED_OVERLAY` coalescé
    (≤ 20 Hz, réémission 250 ms pour tenir le TTL, `OLED_CLEAR` quand plus rien n'est frais). Les rafales de
    restauration sont ignorées (3 s de mutisme au démarrage + `isBulkParamApplyActive()`).
  - **LEDs** — Off (éteinte + appui local inhibé), Press (comportement local, rien envoyé), **Follow**
    (recherche inverse `paramForEvent` sur le MIDI que produit ce bouton → valeur du paramètre : 2 états →
    on/off, continu → luminosité), Manual (`sp3ctraLedNLevel`, automatisable). Envoi sur changement + toutes
    les 2 s (UDP sans accusé).
  - **Salut au bind** : clignotement ×2 des 3 LEDs + overlay « SP3CTRA LINK v<version> » 1,5 s (maintenu
    contre la coalescence de la file, sinon l'état normal l'écrasait 30 ms plus tard).
- Firmware : `LED_SET`/`OLED_OVERLAY` tracés sur l'UART ; **fin de session = nettoyage** (overlay effacé,
  inhibition d'appui local relâchée, LEDs éteintes) — un hôte qui plante ne laisse plus l'instrument avec des
  rétroéclairages morts.
- **Validé sur le CIS** : salut reçu intact, état LED normal 1,5 s après, session fermée par timeout après un
  `kill -9`. Reste à valider de visu par l'utilisateur : overlay d'un paramètre en cours d'édition (mode Chain),
  LED en mode Follow/Manual, boutons/inclinaison appris sur des paramètres.
