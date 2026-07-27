# 🔍 Agents de Revue de Code VST - Sp3ctra

Ce répertoire contient un ensemble d'agents de revue de code spécialement conçus pour analyser le projet VST Sp3ctra et détecter les problèmes architecturaux, duplications, biais de code généré par IA, et incohérences UI.

## 🎯 Objectifs

Les agents vérifient :
- ✅ **Architecture** : Cohérence architecturale, séparation des responsabilités, contraintes RT-audio
- ✅ **Duplications** : Code dupliqué entre standalone et VST, fonctionnalités non-connectées
- ✅ **Biais IA** : Patterns typiques du "vibe coding" assisté par IA (commentaires excessifs, code mort, placeholders)
- ✅ **UI** : Homogénéité de l'interface JUCE (couleurs, polices, layouts, thread-safety)

## 📁 Structure

```
scripts/code_review/
├── run_code_review.py          # 🎬 Orchestrateur principal (POINT D'ENTRÉE)
├── agent_architecture.py       # 🏗️  Agent 1: Architecture Review
├── agent_duplication.py        # 🔁 Agent 2: Code Duplication Detector
├── agent_ai_bias.py           # 🤖 Agent 3: AI Vibe Coding Bias Detector
├── agent_ui_consistency.py    # 🎨 Agent 4: UI Consistency Checker
├── reports/                   # 📋 Rapports générés
│   ├── CONSOLIDATED_REPORT.txt  # Rapport consolidé principal
│   ├── architecture_review_report.txt
│   ├── code_duplication_report.txt
│   ├── ai_bias_detection_report.txt
│   ├── ui_consistency_report.txt
│   └── summary.json           # Résumé JSON (pour CI/CD)
└── README.md                  # 📖 Cette documentation
```

## 🚀 Utilisation

### Exécution Complète (Recommandé)

```bash
# Depuis la racine du projet
python3 scripts/code_review/run_code_review.py
```

Cela exécute tous les agents et génère un rapport consolidé dans `scripts/code_review/reports/`.

### Exécution d'un Agent Individuel

```bash
# Agent 1: Architecture
python3 scripts/code_review/agent_architecture.py

# Agent 2: Duplication
python3 scripts/code_review/agent_duplication.py

# Agent 3: AI Bias
python3 scripts/code_review/agent_ai_bias.py

# Agent 4: UI Consistency
python3 scripts/code_review/agent_ui_consistency.py
```

## 📊 Interprétation des Résultats

### Niveaux de Sévérité

- **❌ ERROR** : Problème critique à corriger avant production
- **⚠️ WARNING** : Problème à adresser pour améliorer la qualité
- **ℹ️ INFO** : Suggestion d'amélioration optionnelle

### Catégories d'Issues

#### 🏗️ Architecture

- **Global State** : Variables globales problématiques pour multi-instance VST
- **Audio Layer** : Utilisation incorrecte de RtAudio dans le VST (devrait être JUCE uniquement)
- **Separation of Concerns** : Responsabilités mal séparées entre composants
- **RT-Audio Safety** : Opérations non-RT dans le callback audio (malloc, mutex, logging)
- **Instance Isolation** : Patterns singleton risquant des conflits entre instances
- **Configuration** : Gestion config (devrait être APVTS, pas .ini)

#### 🔁 Duplication

- **Cross-boundary** : Code dupliqué entre VST et standalone
- **Config Handling** : Configuration gérée en plusieurs endroits
- **Buffer Init** : Initialisation de buffers redondante
- **Unused Code** : Stubs ou fonctions jamais appelées

#### 🤖 AI Bias

- **Generic Comments** : Commentaires AI génériques (TODO, FIXME, Note:)
- **Over-commented** : Ratio commentaires/code > 50%
- **Placeholder Code** : Code incomplet (TODO, juce::ignoreUnused)
- **Inconsistent Naming** : Mélange de camelCase, snake_case, PascalCase
- **Dead Code** : Blocs de code commentés (3+ lignes)
- **Magic Numbers** : Nombres hardcodés sans constante nommée
- **Copy-Paste Pattern** : Blocs de code répétitifs similaires

#### 🎨 UI

- **Color Consistency** : Trop de couleurs uniques (besoin palette/theme)
- **Font Consistency** : Trop de tailles de polices différentes
- **Layout Hardcoding** : setBounds() avec coordonnées fixes (non-responsive)
- **Naming Inconsistency** : Nommage incohérent des composants UI
- **Event Safety** : Accès pointers sans nullptr check dans event handlers
- **Parameter Binding** : Contrôles UI non liés aux paramètres APVTS
- **Thread Safety** : Appels UI depuis le thread audio (processBlock)

## 🎯 Derniers Résultats (2026-01-16)

```
📊 RÉSUMÉ
═══════════════════════════════════════════════════════
Total Issues: 22
  - ❌ Errors:    0  ✅ Excellent !
  - ⚠️ Warnings:  13
  - ℹ️ Info:      9
═══════════════════════════════════════════════════════
```

### 💡 Recommandations Prioritaires

1. **Architecture** : Revoir la séparation des responsabilités (PluginProcessor, Sp3ctraCore, UI)
2. **Duplication** : Centraliser la gestion de la configuration UDP
3. **AI Bias** : Nettoyer les TODOs et magic numbers
4. **UI** : Ajouter des Attachments APVTS pour lier les contrôles aux paramètres

## 🔧 Maintenance

### Ajouter un Nouveau Check

1. Éditer l'agent concerné (ex: `agent_architecture.py`)
2. Ajouter une méthode `check_xxx()`
3. Appeler cette méthode dans `run()`

```python
def check_new_pattern(self):
    """Check for new pattern"""
    print("🔍 Checking new pattern...")
    
    for cpp_file in self.vst_dir.glob("*.cpp"):
        # ... analyse ...
        self.add_issue(
            'Category Name',
            str(cpp_file),
            line_number,
            "Issue description",
            'WARNING'  # ou 'ERROR', 'INFO'
        )
```

### Exécution en CI/CD

Le fichier `summary.json` peut être utilisé pour l'intégration CI/CD :

```bash
# Exemple : Échouer si des erreurs
python3 scripts/code_review/run_code_review.py
if [ $(jq '.summary.errors' scripts/code_review/reports/summary.json) -gt 0 ]; then
    echo "❌ Erreurs critiques détectées !"
    exit 1
fi
```

## 📖 Références

- **VST_Migration_Plan.md** : Plan de migration standalone → VST
- **vst/NOTES_ARCHITECTURE.md** : Notes d'architecture VST vs Standalone

## 🤝 Contribution

Pour améliorer les agents :

1. Identifier un pattern problématique récurrent
2. Créer un check dans l'agent approprié
3. Tester sur le code existant
4. Documenter dans ce README

## 📝 Changelog

- **2026-01-16** : Création initiale des 4 agents + orchestrateur
  - Agent Architecture (contraintes RT, séparation, globals)
  - Agent Duplication (cross-boundary, unused code)
  - Agent AI Bias (vibe coding patterns)
  - Agent UI Consistency (JUCE best practices)

---

**Auteur** : Généré par IA pour le projet Sp3ctra  
**Dernière mise à jour** : 2026-01-16
