# InkPoint X localization coverage

Validation date: 2026-08-06

## Release status

- Supported languages: 28
- Source strings per language: 533
- Localized entries: 14,924
- Missing or empty entries: 0
- English fallback entries: 0
- Locale UI code points: 895
- Generated interface-font files checked: 12

## Supported languages

| Code | Language | Own strings | Fallback |
| --- | --- | ---: | ---: |
| EN | English | 533 | 0 |
| AR | العربية | 533 | 0 |
| BE | Беларуская | 533 | 0 |
| CA | Català | 533 | 0 |
| CAV | Valencià | 533 | 0 |
| CS | Čeština | 533 | 0 |
| DA | Dansk | 533 | 0 |
| DE | Deutsch | 533 | 0 |
| ES | Español | 533 | 0 |
| FI | Suomi | 533 | 0 |
| FR | Français | 533 | 0 |
| HE | עברית | 533 | 0 |
| HU | Magyar | 533 | 0 |
| IT | Italiano | 533 | 0 |
| KK | Қазақша | 533 | 0 |
| KO | 한국어 | 533 | 0 |
| LT | Lietuvių | 533 | 0 |
| NL | Nederlands | 533 | 0 |
| PL | Polski | 533 | 0 |
| PT | Português (Brasil) | 533 | 0 |
| RO | Română | 533 | 0 |
| RU | Русский | 533 | 0 |
| SI | Slovenščina | 533 | 0 |
| SK | Slovenčina | 533 | 0 |
| SV | Svenska | 533 | 0 |
| TR | Türkçe | 533 | 0 |
| UK | Українська | 533 | 0 |
| VI | Tiếng Việt | 533 | 0 |

## Automated gates

`scripts/validate_i18n.py` checks:

- exact key coverage and canonical key order;
- locale metadata and duplicate language codes;
- `printf` placeholder identity;
- leading and trailing whitespace required by composed labels;
- leaked translation-protection tokens and unsupported control characters;
- glyph availability in every generated Inter/Noto UI subset, including
  Arabic contextual forms used by dynamic metadata and Hangul in every
  Korean runtime face.

`scripts/fill_missing_translations.py` is a reproducible audit and fill tool.
It protects firmware names, file formats, protocols, product terminology, and
format placeholders while filling only absent strings. Existing localized
work is retained.

## Release verification

- Release build: passed
- Host tests: 132/132 passed
- Static analysis: passed with no high or medium findings
- Generated-font coverage: 12/12 UI font files passed
- Universal X3/X4 release image: passed the 6.25 MiB OTA-slot budget
