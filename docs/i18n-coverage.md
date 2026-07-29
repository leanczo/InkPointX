# InkPoint X localization coverage

Validation date: 2026-07-25

## Release status

- Supported languages: 26
- Source strings per language: 522
- Localized entries: 13,572
- Missing or empty entries: 0
- English fallback entries: 0
- Locale UI code points: 369
- Selectable interface-font families checked: 3
- Dedicated heading-font stack checked: 1

## Supported languages

| Code | Language | Own strings | Fallback |
| --- | --- | ---: | ---: |
| EN | English | 522 | 0 |
| BE | Беларуская | 522 | 0 |
| CA | Català | 522 | 0 |
| CAV | Valencià | 522 | 0 |
| CS | Čeština | 522 | 0 |
| DA | Dansk | 522 | 0 |
| DE | Deutsch | 522 | 0 |
| ES | Español | 522 | 0 |
| FI | Suomi | 522 | 0 |
| FR | Français | 522 | 0 |
| HE | עברית | 522 | 0 |
| HU | Magyar | 522 | 0 |
| IT | Italiano | 522 | 0 |
| KK | Қазақша | 522 | 0 |
| LT | Lietuvių | 522 | 0 |
| NL | Nederlands | 522 | 0 |
| PL | Polski | 522 | 0 |
| PT | Português (Brasil) | 522 | 0 |
| RO | Română | 522 | 0 |
| RU | Русский | 522 | 0 |
| SI | Slovenščina | 522 | 0 |
| SK | Slovenčina | 522 | 0 |
| SV | Svenska | 522 | 0 |
| TR | Türkçe | 522 | 0 |
| UK | Українська | 522 | 0 |
| VI | Tiếng Việt | 522 | 0 |

## Automated gates

`scripts/validate_i18n.py --all-ui-fonts` checks:

- exact key coverage and canonical key order;
- locale metadata and duplicate language codes;
- `printf` placeholder identity;
- leading and trailing whitespace required by composed labels;
- leaked translation-protection tokens and unsupported control characters;
- glyph availability in every generated FiraGO Medium/SemiBold UI subset,
  including Arabic contextual forms used by dynamic metadata.

`scripts/fill_missing_translations.py` is a reproducible audit and fill tool.
It protects firmware names, file formats, protocols, product terminology, and
format placeholders while filling only absent strings. Existing localized
work is retained.

## Release verification

- Release build: passed
- Host tests: 94/94 passed
- Static analysis: passed with no high or medium findings
- Font bitmap decompression: 45 compressed font files passed
- Device upload: passed on the connected XTEINK X4
- Native framebuffer evidence:
  `docs/qa/inkpoint-all-locales-device.png`
