"""Generate Phase 10.3 offered-load comparison plots."""
import csv, glob, json
from pathlib import Path

ROOT = Path(__file__).resolve().parent
summary = json.loads((ROOT / "experiment-summary.json").read_text())
configs = summary["configurations"]
colors = ("#2878b5", "#e1812c", "#55a868")

def esc(s): return str(s).replace("&", "&amp;").replace("<", "&lt;")
def t(x,y,s,anchor="middle",size=12): return f'<text x="{x:g}" y="{y:g}" text-anchor="{anchor}" font-family="sans-serif" font-size="{size}">{esc(s)}</text>'

def chart(path, title, ylabel, series, labels, suffix=""):
    w,h,L,R,T,B=760,460,78,25,60,90; cw,ch=w-L-R,h-T-B
    ymax=max(v for _,vals in series for v in vals)*1.15 or 1
    out=[f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}">','<rect width="100%" height="100%" fill="white"/>',t(w/2,30,title,size=18),t(20,T+ch/2,ylabel,size=13)]
    for k in range(5):
        y=T+ch*(1-k/4); v=ymax*k/4
        out += [f'<line x1="{L}" y1="{y:g}" x2="{L+cw}" y2="{y:g}" stroke="#ddd"/>',t(L-8,y+4,f"{v:.1f}","end",11)]
    xs=[L+cw*i/(len(labels)-1) for i in range(len(labels))]
    for x,label in zip(xs,labels): out.append(t(x,T+ch+24,str(label)))
    for si,(name,vals) in enumerate(series):
        points=" ".join(f"{x:g},{T+ch-ch*v/ymax:g}" for x,v in zip(xs,vals))
        out += [f'<polyline points="{points}" fill="none" stroke="{colors[si]}" stroke-width="3"/>']
        for x,v in zip(xs,vals): out.append(f'<circle cx="{x:g}" cy="{T+ch-ch*v/ymax:g}" r="4" fill="{colors[si]}"/>')
        lx=180+si*180; out += [f'<line x1="{lx}" y1="410" x2="{lx+22}" y2="410" stroke="{colors[si]}" stroke-width="3"/>',t(lx+28,414,name,"start",11)]
    out += [t(w/2,h-22,"Offered requests per second",size=12),"</svg>"]
    path.write_text("\n".join(out)+"\n")

labels=[int(c["name"].split("-")[1]) for c in configs]
latency=[("p50",[c["aggregate"]["latency_ms"]["p50"]["median"] for c in configs]),("p95",[c["aggregate"]["latency_ms"]["p95"]["median"] for c in configs]),("p99",[c["aggregate"]["latency_ms"]["p99"]["median"] for c in configs])]
chart(ROOT/"latency_vs_offered_load.svg","Latency vs offered load","Latency (ms)",latency,labels)
throughput=[c["aggregate"]["throughput_requests_per_second"]["median"] for c in configs]
chart(ROOT/"throughput_vs_offered_load.svg","Achieved throughput vs offered load","Requests per second",[("achieved",throughput),("offered",labels)],labels)

lag=[]
for c in configs:
    vals=[]
    for p in glob.glob(str(ROOT/c["name"]/"run-*/requests.csv")):
        rows=[r for r in csv.DictReader(open(p)) if r["warmup"].lower()!="true"]
        vals.append(sorted(float(r["dispatch_lag_ms"]) for r in rows)[len(rows)//2])
    lag.append(sum(sorted(vals)[1:2]))
chart(ROOT/"dispatch_lag_vs_offered_load.svg","Median dispatch lag vs offered load","Dispatch lag (ms)",[("median run lag",lag)],labels)
