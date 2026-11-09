#!/usr/bin/env python3
"""
Visualisation (matplotlib, style MATLAB) des CSV enregistrés par record.cpp.

Colonnes attendues :
    t_s, body_id, quality, x_mm, y_mm, z_mm, rx_deg, ry_deg, rz_deg, r0..r8
(r0..r8 = matrice de rotation stockée colonne par colonne, optionnelle)pyton

Usage :
    python plot_record.py enregistrement.csv
    python plot_record.py enregistrement.csv --body 0 2 --unwrap --axes 50
    python plot_record.py                      # ouvre une boîte de dialogue

Dépendances : pip install numpy pandas matplotlib
"""

import argparse
import sys
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
from mpl_toolkits.mplot3d import Axes3D  # noqa: F401  (active la projection 3d)

REQUIRED = ["t_s", "body_id", "quality", "x_mm", "y_mm", "z_mm",
            "rx_deg", "ry_deg", "rz_deg"]
ROT = [f"r{i}" for i in range(9)]


# ---------------------------------------------------------------------------
# Chargement
# ---------------------------------------------------------------------------
def load_csv(path: Path) -> pd.DataFrame:
    """Charge un CSV (séparateur ',' ou ';' détecté, virgule décimale tolérée)."""
    with open(path, "r", encoding="utf-8-sig") as f:
        header = f.readline()
    delim = ";" if header.count(";") > header.count(",") else ","

    df = pd.read_csv(path, sep=delim, dtype=str, encoding="utf-8-sig")
    df.columns = [c.strip() for c in df.columns]

    missing = [c for c in REQUIRED if c not in df.columns]
    if missing:
        raise ValueError(f"Colonnes manquantes dans {path.name} : {', '.join(missing)}")

    cols = REQUIRED + [c for c in ROT if c in df.columns]
    df = df[cols]
    if delim == ";":  # Excel FR : "1,5" -> "1.5"
        df = df.apply(lambda s: s.str.replace(",", ".", regex=False))
    df = df.apply(pd.to_numeric, errors="coerce")

    n_before = len(df)
    df = df.dropna(subset=REQUIRED)
    if len(df) < n_before:
        print(f"[!] {n_before - len(df)} ligne(s) invalide(s) ignorée(s)")
    if df.empty:
        raise ValueError("Aucune ligne valide")

    df["body_id"] = df["body_id"].astype(int)
    return df.sort_values("t_s").reset_index(drop=True)


def choose_file() -> Path:
    """Boîte de dialogue de sélection de fichier (repli : saisie au clavier)."""
    try:
        import tkinter as tk
        from tkinter import filedialog
        root = tk.Tk()
        root.withdraw()
        p = filedialog.askopenfilename(title="Choisir un CSV",
                                       filetypes=[("CSV", "*.csv"), ("Tous", "*.*")])
        root.destroy()
        if p:
            return Path(p)
        sys.exit("Aucun fichier sélectionné")
    except ImportError:
        return Path(input("Chemin du fichier CSV : ").strip().strip('"'))


# ---------------------------------------------------------------------------
# Statistiques
# ---------------------------------------------------------------------------
def print_summary(df: pd.DataFrame) -> None:
    print(f"\nFichier : {len(df)} échantillons, durée {df['t_s'].max():.2f} s, "
          f"{df['body_id'].nunique()} body")
    for bid, d in df.groupby("body_id"):
        dur = d["t_s"].max() - d["t_s"].min()
        rate = (len(d) - 1) / dur if dur > 0 else float("nan")
        print(f"  Body {bid}: {len(d)} éch., {dur:.2f} s, ~{rate:.1f} Hz, "
              f"qualité moy. {d['quality'].mean():.2f}")
        for c in ("x_mm", "y_mm", "z_mm"):
            print(f"      {c:5s} min {d[c].min():10.2f}  max {d[c].max():10.2f}  "
                  f"moy {d[c].mean():10.2f}")


# ---------------------------------------------------------------------------
# Tracés
# ---------------------------------------------------------------------------
def plot_timeseries(d: pd.DataFrame, bid: int, unwrap: bool):
    """Position (colonne gauche) et orientation (colonne droite) en fonction du temps."""
    fig, axes = plt.subplots(3, 2, sharex=True, figsize=(11, 7))
    fig.suptitle(f"Body {bid} - évolution temporelle")
    t = d["t_s"].to_numpy()

    pos = [("x_mm", "X (mm)"), ("y_mm", "Y (mm)"), ("z_mm", "Z (mm)")]
    ori = [("rx_deg", "RX (°)"), ("ry_deg", "RY (°)"), ("rz_deg", "RZ (°)")]

    for i, (col, label) in enumerate(pos):
        ax = axes[i, 0]
        ax.plot(t, d[col], lw=1)
        ax.set_ylabel(label)
        ax.grid(True)
    for i, (col, label) in enumerate(ori):
        ax = axes[i, 1]
        y = d[col].to_numpy()
        if unwrap:  # évite les sauts ±180°
            y = np.rad2deg(np.unwrap(np.deg2rad(y)))
        ax.plot(t, y, lw=1, color="tab:orange")
        ax.set_ylabel(label)
        ax.grid(True)

    axes[2, 0].set_xlabel("t (s)")
    axes[2, 1].set_xlabel("t (s)")
    fig.tight_layout()
    return fig


def _set_axes_equal(ax):
    """Même échelle sur X, Y, Z pour ne pas déformer la trajectoire."""
    lims = np.array([ax.get_xlim3d(), ax.get_ylim3d(), ax.get_zlim3d()])
    center = lims.mean(axis=1)
    radius = 0.5 * (lims[:, 1] - lims[:, 0]).max()
    ax.set_xlim3d(center[0] - radius, center[0] + radius)
    ax.set_ylim3d(center[1] - radius, center[1] + radius)
    ax.set_zlim3d(center[2] - radius, center[2] + radius)


def plot_trajectory(d: pd.DataFrame, bid: int, axes_every: int):
    """Trajectoire 3D colorée par le temps, avec repères d'orientation optionnels."""
    fig = plt.figure(figsize=(8, 7))
    ax = fig.add_subplot(111, projection="3d")
    x, y, z = d["x_mm"].to_numpy(), d["y_mm"].to_numpy(), d["z_mm"].to_numpy()
    t = d["t_s"].to_numpy()

    ax.plot(x, y, z, color="gray", lw=0.7, alpha=0.6)
    sc = ax.scatter(x, y, z, c=t, cmap="viridis", s=6)
    ax.scatter(x[0], y[0], z[0], color="green", s=60, marker="o", label="début")
    ax.scatter(x[-1], y[-1], z[-1], color="red", s=60, marker="s", label="fin")
    fig.colorbar(sc, ax=ax, shrink=0.6, pad=0.1, label="t (s)")

    ax.set_xlabel("X (mm)")
    ax.set_ylabel("Y (mm)")
    ax.set_zlabel("Z (mm)")
    ax.set_title(f"Body {bid} - trajectoire 3D")
    ax.legend(loc="upper left")
    _set_axes_equal(ax)

    # Repères d'orientation : colonnes de R (rot stockée colonne par colonne)
    if axes_every > 0 and all(c in d.columns for c in ROT):
        extent = max(np.ptp(x), np.ptp(y), np.ptp(z), 1.0)
        length = 0.06 * extent
        rot = d[ROT].to_numpy()
        for i in range(0, len(d), axes_every):
            r = rot[i]
            if not np.any(r):  # matrice absente / nulle
                continue
            for j, color in enumerate(("r", "g", "b")):
                v = r[3 * j:3 * j + 3] * length
                ax.quiver(x[i], y[i], z[i], v[0], v[1], v[2], color=color, linewidth=1)
        _set_axes_equal(ax)

    fig.tight_layout()
    return fig


# ---------------------------------------------------------------------------
# Programme principal
# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="Visualise un CSV enregistré par record.cpp")
    ap.add_argument("csv", nargs="?", help="chemin du fichier CSV (sinon boîte de dialogue)")
    ap.add_argument("--body", type=int, nargs="+", help="ne tracer que ces body_id")
    ap.add_argument("--unwrap", action="store_true",
                    help="dérouler les angles pour supprimer les sauts à ±180°")
    ap.add_argument("--axes", type=int, default=0, metavar="N",
                    help="dessiner un repère d'orientation tous les N échantillons (3D)")
    ap.add_argument("--save", metavar="DOSSIER",
                    help="enregistrer les figures en PNG dans ce dossier")
    ap.add_argument("--no-show", action="store_true", help="ne pas ouvrir les fenêtres")
    args = ap.parse_args()

    path = Path(args.csv) if args.csv else choose_file()
    try:
        df = load_csv(path)
    except (OSError, ValueError) as e:
        sys.exit(f"Erreur : {e}")

    print_summary(df)

    ids = sorted(df["body_id"].unique())
    if args.body:
        ids = [b for b in ids if b in args.body]
        if not ids:
            sys.exit("Aucun des body demandés n'est présent dans le fichier")

    out_dir = Path(args.save) if args.save else None
    if out_dir:
        out_dir.mkdir(parents=True, exist_ok=True)

    for bid in ids:
        d = df[df["body_id"] == bid]
        figs = {
            f"{path.stem}_body{bid}_temps.png": plot_timeseries(d, bid, args.unwrap),
            f"{path.stem}_body{bid}_3d.png": plot_trajectory(d, bid, args.axes),
        }
        if out_dir:
            for name, fig in figs.items():
                fig.savefig(out_dir / name, dpi=150)
                print(f"Figure enregistrée : {out_dir / name}")

    if not args.no_show:
        plt.show()


if __name__ == "__main__":
    main()