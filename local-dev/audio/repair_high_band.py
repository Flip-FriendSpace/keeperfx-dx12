# SPDX-License-Identifier: GPL-2.0-or-later
"""Re-export Modern masters with edge-safe cleanup and source-constrained high-frequency detail.

Requires the local sources/, clean/ and modern/ masters; never edits source banks.
"""
import argparse, hashlib, json, math, re, shutil
from pathlib import Path
import numpy as np
import soundfile as sf
from scipy import signal, ndimage
import pyloudnorm as pyln
SR=48000

def level(x):
    try:
        v=float(pyln.Meter(SR,block_size=min(.4,max(.01,len(x)/SR/2))).integrated_loudness(x))
    except ValueError:
        v=float("-inf")
    return v if math.isfinite(v) else float(20*np.log10(max(np.sqrt(np.mean(x*x)),1e-12)))

def peak(x):
    return float(np.max(np.abs(signal.resample_poly(x,4,1,axis=0,window=("kaiser",10)))))

def resample(x, rate):
    g=math.gcd(SR,rate)
    return signal.resample_poly(x,SR//g,rate//g,window=("kaiser",10.))[:round(len(x)*SR/rate)]

def cleanup(x, rate, kind):
    # The v1 spectral noise reduction, without its DC removal and sub-audio high-pass.
    # Those removed nothing audible, but sosfiltfilt's 9-sample edge padding left a
    # step of up to 15% full scale at the edges of many effects: a click on every play.
    reduction=3.5 if kind=="speech" else 1.5
    _,_,z=signal.stft(np.pad(x,(0,max(0,1024-len(x)))),fs=rate,nperseg=1024,noverlap=768,boundary="zeros")
    mag=np.abs(z);energy=np.sqrt(np.mean(mag**2,axis=0))
    quiet=energy<=np.quantile(energy,.15)
    noise=ndimage.gaussian_filter1d(np.median(mag[:,quiet],axis=1),2)[:,None]
    gain=np.maximum((mag**2)/(mag**2+(.65*noise)**2+1e-16),10**(-reduction/20))
    gain=ndimage.gaussian_filter(gain,(1.5,1.))
    _,clean=signal.istft(z*gain,fs=rate,nperseg=1024,noverlap=768,boundary=True)
    return clean[:len(x)]

def edge_safe(result, source):
    # Neither edge may step from silence by more than 1% of peak beyond the original's own step.
    for channel in range(source.shape[1]):
        allowed=.01*np.max(np.abs(source[:,channel]))
        for i in (0,-1):
            assert abs(result[i,channel])<=abs(source[i,channel])+allowed,(i,channel)

def repair(clean, modern, source_rate, kind):
    # Limit only the reconstructed residual. The cleaned original is unchanged.
    start=6500 if kind=="speech" else 7000
    if source_rate>22050:start=max(start,source_rate/2-500)
    n=len(clean);result=np.empty_like(clean);metrics=[]
    for channel in range(clean.shape[1]):
        base=clean[:,channel];detail=modern[:,channel]-base
        pad=max(0,1024-n)
        freq,_,c=signal.stft(np.pad(base,(0,pad)),fs=SR,nperseg=1024,noverlap=768)
        _,_,d=signal.stft(np.pad(detail,(0,pad)),fs=SR,nperseg=1024,noverlap=768)
        # No new tonal peak may dominate the original at the same time/frequency.
        cap=np.minimum(1.,.15*np.abs(c)/(np.abs(d)+1e-20))
        cap=np.minimum(cap,ndimage.gaussian_filter(cap,(1.,1.)))
        low=np.clip((freq-start)/1500,0,1);low=.5-.5*np.cos(np.pi*low)
        upper=min(source_rate/2,20000.)
        high=np.clip((upper-freq)/1500,0,1);high=.5-.5*np.cos(np.pi*high)
        _,filtered=signal.istft(d*cap*low[:,None]*high[:,None],fs=SR,nperseg=1024,noverlap=768)
        filtered=filtered[:n]
        # Preserve the protected original band exactly, including short effects.
        f=np.fft.rfftfreq(n,1/SR);r=np.fft.rfft(filtered)
        protect=np.clip((f-start)/500,0,1);protect=.5-.5*np.cos(np.pi*protect)
        r*=protect
        filtered=np.fft.irfft(r,n=n)
        result[:,channel]=base+filtered
        below=f<start
        err=np.sqrt(np.sum(np.abs(np.fft.rfft(filtered)[below])**2)/(np.sum(np.abs(np.fft.rfft(base)[below])**2)+1e-20))
        assert err<2e-5
        metrics.append(float(err))
    assert np.isfinite(result).all()
    return result,max(metrics)

def main():
    ap=argparse.ArgumentParser();ap.add_argument("--master-directory",type=Path,required=True);ap.add_argument("--package-directory",type=Path,required=True);ap.add_argument("--output-directory",type=Path,required=True);a=ap.parse_args()
    assert not a.output_directory.exists(), "Output already exists; preserve prior evidence"
    a.output_directory.mkdir(parents=True)
    manifest=json.loads((a.package_directory/"local-audio-mod.json").read_text())
    plan=json.loads((a.master_directory/"plan.json").read_text());jobs={j["key"]:j for j in plan["jobs"]}
    for name in ["sounds.cfg","fxdata/sounds.cfg","NOTICE.md"]:
        dst=a.output_directory/name;dst.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(a.package_directory/name,dst)
    lookup={(e["type"],e.get("name") if e["type"]=="campaign" else e["id"]):e["job"] for e in plan["entries"]}
    cache={};changes=[]
    for i,e in enumerate(manifest["files"]):
        key=lookup[(e["type"],e["name"] if e["type"]=="campaign" else e["id"])];job=jobs[key]
        if key not in cache:
            clean,sr=sf.read(a.master_directory/"clean"/(key+".wav"),always_2d=True)
            modern,sr2=sf.read(a.master_directory/"modern"/(key+".wav"),always_2d=True)
            assert sr==sr2==SR and clean.shape==modern.shape
            source,rate=sf.read(a.master_directory/"sources"/(key+".wav"),dtype="float64",always_2d=True)
            assert rate==job["source_rate"] and source.shape[1]==clean.shape[1]
            base=np.column_stack([resample(cleanup(source[:,c],rate,job["kind"]),rate) for c in range(source.shape[1])])
            assert base.shape==clean.shape
            # Rebase the v1 reconstructed detail onto the edge-safe cleanup.
            master,error=repair(base,base+(modern-clean),job["source_rate"],job["kind"])
            edge_safe(master,np.column_stack([resample(source[:,c],rate) for c in range(source.shape[1])]))
            cache[key]=master,error
        master,error=cache[key];requested=10**((e["target_lufs"]-level(master))/20)
        gain=min(requested,10**(-1.05/20)/max(peak(master),1e-12));y=master*gain
        rng=np.random.default_rng(int(hashlib.sha256(e["file"].encode()).hexdigest()[:16],16));y=y+(rng.random(y.shape)-rng.random(y.shape))/65536
        dest=a.output_directory/e["file"];dest.parent.mkdir(parents=True,exist_ok=True);sf.write(dest,y,SR,subtype="PCM_16")
        decoded,rate=sf.read(dest,always_2d=True);pk=peak(decoded)
        assert rate==SR and decoded.shape==(e["frames"],e["channels"]) and pk<10**(-1/20)
        changes.append(dict(file=e["file"],previous_sha256=e["sha256"]))
        e.update(sha256=hashlib.sha256(dest.read_bytes()).hexdigest(),output_lufs=level(decoded),gain_db=float(20*np.log10(gain)),headroom_trim_db=float(20*np.log10(gain/requested)),true_peak_dbfs=float(20*np.log10(max(pk,1e-12))),protected_band_relative_error=error)
        e["reconstruction"]=dict(source="Modern v1 aligned residual",maximum_residual_to_source_magnitude=.15,upper_band_end_hz=min(job["source_rate"]/2,20000),source_rate=job["source_rate"],protected_hz=(6500 if job["kind"]=="speech" else 7000) if job["source_rate"]<=22050 else max(6500 if job["kind"]=="speech" else 7000,job["source_rate"]/2-500))
        if i%50==0:print(f"Repaired {i+1}/{len(manifest['files'])}",flush=True)
    manifest.update(treatment="Modern v2.1 edge-safe cleanup, source-constrained high band",strength=None,cleanup_policy="Spectral noise reduction only, at most 3.5 dB for speech and 1.5 dB for effects. No DC removal or sub-audio high-pass, so recording edges match the original without added clicks.",high_band_policy="Reconstructed residual capped at 15% of cleaned source STFT magnitude; no added band beyond original Nyquist/20 kHz. Protected original band retained.",mix_policy="Match current game recording loudness with at least -1 dB true-peak headroom; no compression.")
    (a.output_directory/"local-audio-mod.json").write_text(json.dumps(manifest,indent=2)+"\n")
    notice=a.output_directory/"NOTICE.md";text=re.sub(r"audio v[0-9.]+$","audio v2.1",notice.read_text(),count=1,flags=re.M)
    text=re.sub(r"^Treatment: .*$","Treatment: gentle spectral noise reduction and 48 kHz conversion, with source-constrained upper-band detail. Reconstructed detail is capped at 15% of the cleaned original's local spectral magnitude and fades out at the source bandwidth. The lower-band source signal is retained, and no DC or sub-audio filtering alters the start or end of a recording. Loudness follows the current game recordings with -1 dB true-peak headroom. Four distinct noisy recordings retain cleanup only after reconstruction failed alignment checks. The manifest records each file's processing and measurements.",text,count=1,flags=re.M)
    notice.write_text(text)
    (a.output_directory.parent/"audio-repair-report.json").write_text(json.dumps(dict(files=len(changes),unique_masters=len(cache),previous_files=changes,maximum_true_peak_dbfs=max(e["true_peak_dbfs"] for e in manifest["files"]),maximum_protected_band_error=max(e["protected_band_relative_error"] for e in manifest["files"]),recipe=manifest["high_band_policy"],cleanup=manifest["cleanup_policy"]),indent=2))
    print(f"PASS: repaired and validated {len(changes)} WAVs",flush=True)
if __name__=="__main__":main()
