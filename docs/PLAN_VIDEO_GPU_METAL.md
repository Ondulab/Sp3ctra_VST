# VIDEO MIX sur GPU (Metal) — plan d'implémentation

État : **plan, non implémenté.** Les points 1 à 6 du chantier d'optimisation
vidéo (2026-09-04) sont en place et vérifiés ; ce document décrit la seule étape
restante, structurelle, et les conditions dans lesquelles elle vaut la peine.

## Pourquoi ce document n'est pas du code

Le portage GPU remplace un chemin de rendu qui fonctionne, qui est réglé
finement (loi de couleur, warp, atténuation, flou) et qui vient d'être vérifié
par équivalence. Un shader ne se valide pas hors application : il faut voir
l'image. Livrer plusieurs milliers de lignes de Metal non exécutées à la place
d'un rendu correct serait un mauvais échange. À décider par l'auteur du projet,
sur mesure, une fois les gains CPU constatés en session.

## Quand le déclencher

Après les points 1 à 6, mesurer en session réelle le temps d'une passe de rendu
(thread `VideoMixRender`) pendant un enregistrement 2160p avec le nombre de
sorties réellement utilisé. Le portage ne se justifie que si les deux
conditions sont réunies :

- la passe dépasse durablement 16 ms, donc le waterfall n'atteint plus 60 fps ;
- le plafond de résolution (`kMaxRenderDim`) ne peut pas être baissé sans perte
  visible, c'est-à-dire que la sortie est réellement regardée en 4K.

Avec une seule sortie et un plafond à 1600 de diagonale, le CPU suffit
largement. Le GPU se justifie pour du multi-sorties en 4K.

## Architecture cible

Le pipeline garde exactement la même sémantique ; seul le substrat change.

| Étage | Aujourd'hui | Cible Metal |
|---|---|---|
| Historique | `juce::Image` ARGB, deux anneaux de lignes | `MTLTexture` 2D, mêmes deux anneaux, `replaceRegion` des seules lignes neuves |
| Ligne source | `buildLineImage` sur CPU | inchangé (quelques lignes par tick), téléversé |
| Warp + âge + flou + gamma | `buildWarp`, boucles par pixel | un `MTLComputePipelineState` par passe, ou une seule passe fusionnée |
| Zoom / rotation | `videoblit::affineARGB` | quad texturé, échantillonneur linéaire |
| Composite des couches | `blendLayer` | passe de rendu, `MTLBlendFactor` par mode |
| Sortie enregistrement | `CVPixelBuffer` + copie | texture adossée à un `IOSurface`, zéro copie vers VideoToolbox |
| Affichage | `juce::Graphics` → CoreGraphics | `CAMetalLayer` sous une vue JUCE dédiée |

### Points d'intégration

- `VideoScrollRenderCore` : extraire une interface `IWaterfallBackend` avec
  `uploadLines / advance / warp / draw`. L'implémentation CPU actuelle devient
  `WaterfallBackendCpu`, la nouvelle `WaterfallBackendMetal`. Le repli CPU doit
  rester compilable et sélectionnable : c'est la porte de sortie si un pilote
  ou une machine pose problème, et le seul chemin sous Windows.
- `VideoMixerComponent::Renderer` : le pool triple-tampon devient un pool de
  textures ; `front_` devient une `id<MTLTexture>` plus son `IOSurface`.
- `VideoRecorder` : `CVPixelBufferPool` créé avec
  `kCVPixelBufferMetalCompatibilityKey`, la texture de sortie est créée par
  `CVMetalTextureCacheCreateTextureFromImage` sur le buffer du pool. Le
  compositeur écrit alors directement dans ce que l'encodeur va lire.
- Présentation : une `juce::NSViewComponent` hébergeant une `CAMetalLayer`
  remplace le blit du thread message. C'est ce qui supprime le dernier coût
  d'interface, y compris en 5K plein écran.

### Le shader de warp

La loi est déjà exprimée par ligne d'écran, donc elle se transpose telle quelle :
un thread par pixel de sortie, l'index de ligne d'historique calculé par la même
carte `bufEdge`, la moyenne de boîte remplacée par un `mipmap` ou une somme de
préfixes en texture. Attention à trois équivalences à préserver exactement,
sous peine de changer l'image :

- la loi de couleur (`applyDisplayColour`) et le point noir du papier ;
- la courbe d'atténuation `videoScrollAttenLevel`, à porter à l'identique ou à
  passer en table 1D ;
- le flou à rayon fractionnaire, dont la continuité entre lignes est ce qui
  évite les marches d'escalier.

## Risques

- **Verrouillage plateforme.** Le chemin CPU doit rester le chemin de référence
  pour Windows et pour tout repli.
- **Divergence visuelle.** Prévoir un mode de comparaison qui rend la même
  trame par les deux chemins et affiche la différence, sinon les écarts se
  découvrent en concert.
- **Contention GPU.** L'interface JUCE, le pad VIEWPORT et le warp se
  partagent le même appareil ; surveiller la latence de présentation.
- **Cycle de vie du plugin.** Un hôte peut ouvrir et fermer l'éditeur en
  boucle : l'appareil et les pipelines doivent survivre à l'éditeur, comme le
  recorder appartient déjà au processeur.

## Ordre de travail suggéré

1. Instrumenter : publier le temps de passe et le nombre d'images perdues.
2. Extraire `IWaterfallBackend` sans changer de comportement.
3. Backend Metal : historique en texture et warp seuls, sortie relue en CPU,
   comparaison avec le chemin CPU sur trame figée.
4. Composite et zoom/rotation sur GPU.
5. `IOSurface` partagé avec VideoToolbox.
6. `CAMetalLayer` pour l'affichage.
