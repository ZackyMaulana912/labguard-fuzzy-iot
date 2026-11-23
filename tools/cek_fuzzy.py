"""
Cek konsistensi fuzzy antara sketch.ino (C++) dan fuzzy_kelas.py (Python).
Butuh: g++ dan scikit-fuzzy. Jalankan dari root repo: python3 tools/cek_fuzzy.py
"""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "python"))
from fuzzy_kelas import hitung, label, TITIK_UJI  # noqa: E402

sketch = open(os.path.join(ROOT, "wokwi", "sketch.ino")).read()
blok = sketch.split("// FUZZY-BEGIN")[1].split("// FUZZY-END")[0]
titik = ",".join(f"{{{r},{c}}}" for r, c in TITIK_UJI)
kode = ("#include <math.h>\n#include <stdio.h>\n" + blok +
        f"int main(){{float p[][2]={{{titik}}};"
        f"for(int i=0;i<{len(TITIK_UJI)};i++){{float v=hitungFuzzy(p[i][0],p[i][1]);"
        "printf(\"%.3f %d\\n\",v,labelStatus(v));}}")

with tempfile.TemporaryDirectory() as tmp:
    src, exe = os.path.join(tmp, "t.cpp"), os.path.join(tmp, "t")
    open(src, "w").write(kode)
    subprocess.run(["g++", "-Wall", "-O2", src, "-o", exe], check=True)
    hasil = subprocess.run([exe], capture_output=True, text=True, check=True).stdout.split("\n")

NAMA = ["AMAN", "WASPADA", "BAHAYA"]
gagal = 0
for (r, c), baris in zip(TITIK_UJI, hasil):
    v_cpp, st_cpp = baris.split()
    v_py = hitung(r, c)
    cocok = NAMA[int(st_cpp)] == label(v_py) and abs(float(v_cpp) - v_py) < 1.0
    gagal += not cocok
    print(f"RH {r:>3} CO2 {c:>5}  C++ {float(v_cpp):6.2f} {NAMA[int(st_cpp)]:8s} "
          f"Python {v_py:6.2f} {label(v_py):8s} {'v' if cocok else 'x'}")
print("SEMUA COCOK" if gagal == 0 else f"{gagal} TITIK TIDAK COCOK")
sys.exit(1 if gagal else 0)
