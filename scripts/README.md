# Export de la distribution de latence

Le script `export_latency_png.sh` interroge Prometheus et genere une image PNG
de la distribution exacte des latences SV.

## Dépendances

- `bash`
- `curl`
- `jq`
- `gnuplot`

## Commande

Depuis la racine du dépôt :

```bash
./scripts/export_latency_png.sh \
  --prometheus http://192.168.216.168:19090 \
  --output /tmp/sv-latency.png \
  --appid 0x4000 \
  --svid svID0008 \
  --xmax 1100
```

Cette commande cree `/tmp/sv-latency.png`. La ligne verticale rouge en
pointilles a `500 us` est ajoutee automatiquement au graphique.

## Options

| Option | Description | Valeur par défaut |
|---|---|---|
| `--prometheus URL` | URL de base de Prometheus | `http://192.168.216.168:19090` |
| `--output FILE` | Fichier PNG de sortie | `latency-distribution.png` |
| `--appid APPID` | Label `appid` du flux SV | `0x4000` |
| `--svid SVID` | Label `svid` du flux SV | `svID0008` |
| `--xmax US` | Valeur maximale de l'axe X, en microsecondes | aucune |

Le script utilise le compteur cumulatif
`sv_capture_latency_us_observations_total` et `last_over_time` sur les 30
derniers jours. Les observations restent donc exportables apres l'arret de
`sv-subscriber`, tant qu'elles sont encore dans la retention Prometheus,
y compris celles superieures a `1000 us`.

Pour afficher l’aide :

```bash
./scripts/export_latency_png.sh --help
```
