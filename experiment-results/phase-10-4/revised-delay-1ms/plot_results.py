"""Generate plots for the revised Phase 10.4 comparison."""
import csv, glob, json, math
from pathlib import Path

ROOT = Path(__file__).resolve().parent
summary = json.loads((ROOT / "experiment-summary.json").read_text())
configs = summary["configurations"]
colors = ("#2878b5", "#e1812c")

def esc(v): return str(v).replace("&", "&amp;").replace("<", "&lt;")
def text(x,y,v,anchor="middle",size=12): return f'<text x="{x:g}" y="{y:g}" text-anchor="{anchor}" font-family="sans-serif" font-size="{size}">{esc(v)}</text>'

def bars(path, title, ylabel, values, suffix=""):
    w,h,L,R,T,B=680,430,80,25,60,90; cw,ch=w-L-R,h-T-B; ymax=max(values)*1.2 or 1
    out=[f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}">','<rect width="100%" height="100%" fill="white"/>',text(w/2,30,title,size=18),text(20,T+ch/2,ylabel,size=13)]
    for k in range(5):
        y=T+ch*(1-k/4); v=ymax*k/4; out += [f'<line x1="{L}" y1="{y:g}" x2="{L+cw}" y2="{y:g}" stroke="#ddd"/>',text(L-8,y+4,f"{v:.1f}","end",11)]
    for i,(v,c) in enumerate(zip(values,configs)):
        x=L+cw*(i+.5)/len(values); bh=ch*v/ymax; y=T+ch-bh; label=c["name"]
        out += [f'<rect x="{x-70:g}" y="{y:g}" width="140" height="{bh:g}" fill="{colors[i]}"/>',text(x,y-6,f"{v:.2f}"),text(x,T+ch+24,label,size=11)]
    out += [text(w/2,h-22,suffix,size=12),"</svg>"]; path.write_text("\n".join(out)+"\n")

latency=[]
for metric in ("p50","p95","p99"):
    latency.append([c["aggregate"]["latency_ms"][metric]["median"] for c in configs])
w,h,L,R,T,B=760,450,80,25,60,90; cw,ch=w-L-R,h-T-B; ymax=max(v for row in latency for v in row)*1.15
out=[f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}">','<rect width="100%" height="100%" fill="white"/>',text(w/2,30,"Latency percentiles by workload mode",size=18),text(20,T+ch/2,"Latency (ms)",size=13)]
for k in range(5):
    y=T+ch*(1-k/4); v=ymax*k/4; out += [f'<line x1="{L}" y1="{y:g}" x2="{L+cw}" y2="{y:g}" stroke="#ddd"/>',text(L-8,y+4,f"{v:.1f}","end",11)]
for j,metric in enumerate(("p50","p95","p99")):
    center=L+cw/3*(j+.5); out.append(text(center,T+ch+24,metric))
    for i,v in enumerate(latency[j]):
        bw=44; x=center+(i-.5)*52-bw/2; bh=ch*v/ymax; y=T+ch-bh; out += [f'<rect x="{x:g}" y="{y:g}" width="{bw}" height="{bh:g}" fill="{colors[i]}"/>',text(x+bw/2,y-5,f"{v:.2f}",size=10)]
for i,c in enumerate(configs):
    x=220+i*210; out += [f'<rect x="{x}" y="405" width="14" height="14" fill="{colors[i]}"/>',text(x+21,416,c["name"],"start",11)]
out.append("</svg>"); (ROOT/"latency_percentiles.svg").write_text("\n".join(out)+"\n")

bars(ROOT/"throughput.svg","Runner throughput by workload mode","Requests per second",[c["aggregate"]["throughput_requests_per_second"]["median"] for c in configs],"Workload mode")
rates=[]
for c in configs:
    values=[]
    for p in glob.glob(str(ROOT/c["name"]/"run-*/requests.csv")):
        rows=list(csv.DictReader(open(p))); first=float(rows[0]["scheduled_arrival_ms"]); last=float(rows[-1]["scheduled_arrival_ms"]); values.append((len(rows)-1)*1000/(last-first))
    rates.append(sorted(values)[1])
bars(ROOT/"effective_generation_rate.svg","Effective scheduled generation rate","Requests per second",rates,"Workload mode")
