"""
Uji logika fuzzy: Sistem Peringatan Kelembapan dan CO2 Lab Komputer
Jalankan: python3 fuzzy_kelas.py
Hasil (folder output/): grafik fungsi keanggotaan, grafik permukaan, tabel uji
"""
import os
import numpy as np
import skfuzzy as fuzz
from skfuzzy import control as ctrl
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "output")
os.makedirs(OUT, exist_ok=True)

# ---------------- Variabel ----------------
rh = ctrl.Antecedent(np.arange(0, 100.1, 0.1), "kelembapan")   # %
co2 = ctrl.Antecedent(np.arange(400, 3001, 1), "co2")           # ppm
status = ctrl.Consequent(np.arange(0, 101, 1), "status", defuzzify_method="centroid")

rh["normal"] = fuzz.trapmf(rh.universe, [0, 0, 50, 60])
rh["tinggi"] = fuzz.trimf(rh.universe, [50, 65, 80])
rh["sangat_tinggi"] = fuzz.trapmf(rh.universe, [70, 80, 100, 100])

co2["baik"] = fuzz.trapmf(co2.universe, [400, 400, 700, 900])
co2["sedang"] = fuzz.trimf(co2.universe, [700, 1000, 1300])
co2["buruk"] = fuzz.trapmf(co2.universe, [1000, 1400, 3000, 3000])

status["aman"] = fuzz.trapmf(status.universe, [0, 0, 25, 45])
status["waspada"] = fuzz.trimf(status.universe, [30, 50, 70])
status["bahaya"] = fuzz.trapmf(status.universe, [55, 75, 100, 100])

# ---------------- 9 aturan (sama persis dengan sketch.ino) ----------------
#                 CO2 baik   CO2 sedang  CO2 buruk
# RH normal        aman       aman        waspada
# RH tinggi        waspada    waspada     bahaya
# RH sangat tinggi bahaya     bahaya      bahaya
RULES = [
    ("normal", "baik", "aman"),
    ("normal", "sedang", "aman"),
    ("normal", "buruk", "waspada"),
    ("tinggi", "baik", "waspada"),
    ("tinggi", "sedang", "waspada"),
    ("tinggi", "buruk", "bahaya"),
    ("sangat_tinggi", "baik", "bahaya"),
    ("sangat_tinggi", "sedang", "bahaya"),
    ("sangat_tinggi", "buruk", "bahaya"),
]
sistem = ctrl.ControlSystem([ctrl.Rule(rh[a] & co2[b], status[c]) for a, b, c in RULES])


def hitung(kelembapan, co2_ppm):
    sim = ctrl.ControlSystemSimulation(sistem)
    sim.input["kelembapan"] = float(np.clip(kelembapan, 0, 100))
    sim.input["co2"] = float(np.clip(co2_ppm, 400, 3000))
    sim.compute()
    return sim.output["status"]


def label(nilai):
    if nilai >= 65:
        return "BAHAYA"
    if nilai >= 35:
        return "WASPADA"
    return "AMAN"


# Titik uji, juga dipakai untuk membandingkan dengan hasil kode ESP32
TITIK_UJI = [(45, 500), (55, 950), (62, 600), (68, 1000), (58, 1600),
             (75, 800), (85, 600), (72, 1400), (90, 2000), (50, 1200)]

if __name__ == "__main__":
    fig, ax = plt.subplots(3, 1, figsize=(8, 9))
    for a, var, judul in zip(ax, [rh, co2, status],
                             ["Kelembapan (%)", "CO2 (ppm)", "Status ruangan (0 sampai 100)"]):
        for (nama, term), gaya in zip(var.terms.items(), ["-", "--", ":"]):
            a.plot(var.universe, term.mf, gaya, color="black",
                   label=nama.replace("_", " "), linewidth=1.8)
        a.set_title(judul)
        a.set_ylim(-0.05, 1.05)
        a.legend(loc="center right")
        a.grid(alpha=0.3)
    plt.tight_layout()
    plt.savefig(os.path.join(OUT, "fungsi_keanggotaan.png"), dpi=150)
    plt.close()

    xs = np.linspace(30, 100, 36)
    ys = np.linspace(400, 2500, 36)
    Z = np.array([[hitung(x, y) for x in xs] for y in ys])
    fig = plt.figure(figsize=(7, 5))
    a = fig.add_subplot(projection="3d")
    X, Y = np.meshgrid(xs, ys)
    a.plot_surface(X, Y, Z, cmap="Greys", edgecolor="none")
    a.set_xlabel("Kelembapan (%)")
    a.set_ylabel("CO2 (ppm)")
    a.set_zlabel("Status")
    a.set_title("Permukaan keputusan fuzzy")
    plt.tight_layout()
    plt.savefig(os.path.join(OUT, "permukaan_fuzzy.png"), dpi=150)
    plt.close()

    baris = ["kelembapan,co2,nilai_status,label"]
    print(f"{'RH%':>5s} {'CO2':>6s} {'Nilai':>7s}  Status")
    for r, c in TITIK_UJI:
        v = hitung(r, c)
        print(f"{r:5.0f} {c:6.0f} {v:7.2f}  {label(v)}")
        baris.append(f"{r},{c},{v:.2f},{label(v)}")
    with open(os.path.join(OUT, "hasil_uji.csv"), "w") as f:
        f.write("\n".join(baris) + "\n")
    print("\nGrafik dan tabel tersimpan di folder output/")
