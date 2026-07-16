#!/usr/bin/env python3
"""Processes one queued compat-report submission end-to-end: reads
compat-reports/pending/<id>/{meta.json,launcher_log.txt,compat_log.txt,
core_log.txt}, downloads the linked APK, reads its package/version/name via
androguard, extracts an icon, checks the Play Store category, runs an
automated pass over the three logs, and publishes the result to
compat-reports/reports/<package>/<version>/ (overwriting any older
submission for the same package+version) plus data/games.json — then
removes the pending/ entry.

Triggered by a repository_dispatch fired by the Cloudflare Worker that
relays the website's submission form (see compat-reports/README.md and the
website's worker/ folder) — there's no GitHub issue involved, so there's no
comment/close/lock step; failures are surfaced by this workflow run showing
red in the Actions tab, plus an error.txt left next to the pending
submission instead of deleting it, so nothing quietly disappears.
"""
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timezone

VERDICT_LABEL = {
    "ok":      "✅ Playable",
    "issues":  "⚠️ Runs with issues",
    "fail":    "❌ Fails to launch",
    "unknown": "❓ Inconclusive",
}

STALL_RE = re.compile(r'(stall|STALL\(severe\)): frame (\d+) stalled for (\d+)ms')
ENV_BUILD_RE = re.compile(r'env: Android Horizon \(Translation Core\) build (\S+)')
ENV_FIRMWARE_RE = re.compile(r'env: Switch firmware (\S+)')
ENV_ATMOSPHERE_RE = re.compile(r'env: Atmosphere (\S+)')
LOGGED_APK_SHA256_RE = re.compile(r'launchApk: apk sha256=([0-9a-f]{64})')
FRAME_EVIDENCE_RE = re.compile(r'\bframe\s+\d+\b', re.IGNORECASE)
ERROR_MARKERS = [
    "abort", "fatal", "unresolved symbol", "font load failed",
    "envsetnextload failed", "core nro missing", "segmentation fault",
    "unhandled exception", "software closed", "failed to load",
]
DENSITY_ORDER = ["xxxhdpi", "xxhdpi", "xhdpi", "hdpi", "mdpi", "ldpi", "anydpi", "nodpi"]


def safe_path_component(s: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.\-]", "_", s.strip())[:100] or "unknown"


# ------------------------------------------------------------------ steps --

def download_apk(url, dest, max_bytes=300 * 1024 * 1024, timeout=180):
    try:
        result = subprocess.run(
            ["curl", "-sL", "--max-time", str(timeout), "--max-filesize", str(max_bytes),
             "-w", "%{http_code}", "-o", dest, url],
            capture_output=True, text=True, timeout=timeout + 30,
        )
    except subprocess.TimeoutExpired:
        return False, None, "download timed out"
    if result.returncode != 0:
        return False, None, f"curl exit code {result.returncode} ({result.stderr.strip()[:200]})"
    http_code = result.stdout.strip()
    if http_code != "200":
        return False, http_code, f"server responded HTTP {http_code}"
    if not os.path.exists(dest) or os.path.getsize(dest) == 0:
        return False, http_code, "downloaded file is empty"
    return True, http_code, None


def read_apk_metadata(apk_path):
    """Returns (package, version_name, app_name, error). Reads these straight
    out of the compiled AndroidManifest.xml/resources via androguard, rather
    than trusting hand-typed form fields that could drift from what was
    actually tested."""
    import logging
    logging.getLogger("androguard").setLevel(logging.ERROR)
    try:
        from androguard.core.apk import APK
        a = APK(apk_path)
        package = a.get_package()
        version_name = a.get_androidversion_name()
        app_name = a.get_app_name()
    except Exception as e:
        return None, None, None, f"couldn't read the APK's manifest ({e})"
    if not package:
        return None, None, None, "couldn't find a package name in the APK's manifest"
    return package, (version_name or "unknown"), (app_name or package), None


def _resolve_adaptive_icon_layer(apk_obj, axml_bytes):
    """Adaptive icons (<adaptive-icon>, API 26+) point at separate
    foreground/background layers instead of one bitmap. Those layers are
    usually VectorDrawable XML too (not worth rasterizing path data for
    this), but occasionally are plain PNG/WEBP — try the foreground layer,
    then background, and use whichever first resolves to an actual bitmap."""
    from androguard.core.axml import AXMLPrinter
    try:
        xml_text = AXMLPrinter(axml_bytes).get_xml().decode("utf-8", errors="ignore")
    except Exception:
        return None
    arsc = apk_obj.get_android_resources()
    for tag in ("foreground", "background"):
        m = re.search(rf'<{tag}[^>]*android:drawable="@([0-9A-Fa-f]+)"', xml_text)
        if not m:
            continue
        try:
            configs = arsc.get_resolved_res_configs(int(m.group(1), 16))
        except Exception:
            continue
        if not configs:
            continue
        layer_path = configs[0][1]
        if layer_path.lower().endswith((".png", ".webp")):
            try:
                return apk_obj.get_file(layer_path), (".png" if layer_path.lower().endswith(".png") else ".webp")
            except Exception:
                continue
    return None


def extract_icon(apk_path, out_path_no_ext):
    icon_bytes, icon_ext = None, None

    try:
        import logging
        logging.getLogger("androguard").setLevel(logging.ERROR)
        from androguard.core.apk import APK
        a = APK(apk_path)
        icon_res = a.get_app_icon()
        if icon_res and not icon_res.lower().endswith(".xml"):
            data = a.get_file(icon_res)
            if data:
                icon_bytes = data
                icon_ext = ".png" if icon_res.lower().endswith(".png") else ".webp"
        elif icon_res:
            resolved = _resolve_adaptive_icon_layer(a, a.get_file(icon_res))
            if resolved:
                icon_bytes, icon_ext = resolved
    except Exception:
        pass

    if icon_bytes is None:
        import zipfile
        try:
            with zipfile.ZipFile(apk_path) as z:
                candidates = []
                for name in z.namelist():
                    lower = name.lower()
                    if not (lower.startswith("res/mipmap") or lower.startswith("res/drawable")):
                        continue
                    base = lower.rsplit("/", 1)[-1]
                    if not (base.endswith(".png") or base.endswith(".webp")):
                        continue
                    if "ic_launcher" not in base and "icon" not in base:
                        continue
                    density_rank = 99
                    for i, d in enumerate(DENSITY_ORDER):
                        if d in lower:
                            density_rank = i
                            break
                    candidates.append((density_rank, -z.getinfo(name).file_size, name))
                if candidates:
                    candidates.sort()
                    best = candidates[0][2]
                    icon_bytes = z.read(best)
                    icon_ext = ".png" if best.lower().endswith(".png") else ".webp"
        except zipfile.BadZipFile:
            pass

    if icon_bytes is None:
        return None
    out_path = out_path_no_ext + icon_ext
    with open(out_path, "wb") as f:
        f.write(icon_bytes)
    return out_path


def check_play_store_category(package):
    url = f"https://play.google.com/store/apps/details?id={urllib.parse.quote(package)}&hl=en&gl=US"
    req = urllib.request.Request(url, headers={
        "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124 Safari/537.36",
    })
    try:
        with urllib.request.urlopen(req, timeout=20) as resp:
            status, html = resp.status, resp.read().decode("utf-8", errors="ignore")
    except urllib.error.HTTPError as e:
        return "NOT_FOUND" if e.code == 404 else "UNKNOWN"
    except Exception:
        return "UNKNOWN"
    if status != 200:
        return "UNKNOWN"
    if "/store/apps/category/GAME" in html:
        return "GAME"
    if "/store/apps/category/" in html:
        return "NOT_GAME"
    return "UNKNOWN"


def parse_env_header(compat_log):
    """Reads the env: header AHNX-Translation-Core logs at the top of
    compat_log.txt (build/firmware/Atmosphere — see loader.cpp). Older
    submissions predate this header and simply won't match; callers should
    render that as an explicit "unknown", not guess a value."""
    build_m = ENV_BUILD_RE.search(compat_log)
    firmware_m = ENV_FIRMWARE_RE.search(compat_log)
    atmosphere_m = ENV_ATMOSPHERE_RE.search(compat_log)
    return {
        "engine_build": build_m.group(1) if build_m else None,
        "switch_firmware": firmware_m.group(1) if firmware_m else None,
        "atmosphere_version": atmosphere_m.group(1) if atmosphere_m else None,
    }


def check_apk_integrity(compat_log, submitted_apk_sha256):
    """Cross-checks the submitted APK's hash (computed fresh from the just-
    downloaded APK, see apk_sha256 in main()) against the hash the engine
    itself logged for the APK it actually ran (see apkSha256Cached in
    AHNX-Translation-Core's loader.cpp) — confirms the attached logs really
    came from the APK this report claims, not a different build submitted
    under the same link/upload."""
    m = LOGGED_APK_SHA256_RE.search(compat_log)
    if not m:
        return "not_logged", None
    logged = m.group(1)
    return ("match" if logged == submitted_apk_sha256 else "mismatch"), logged


def analyze_logs(launcher_log, compat_log, core_log):
    combined = "\n".join([launcher_log, compat_log, core_log])
    stalls, severe = [], 0
    for m in STALL_RE.finditer(combined):
        kind, ms = m.group(1), int(m.group(3))
        stalls.append(ms)
        if kind.startswith("STALL"):
            severe += 1

    first_error = None
    for line in combined.split("\n"):
        low = line.lower()
        if any(marker in low for marker in ERROR_MARKERS):
            first_error = line.strip()
            break

    frame_evidence = bool(FRAME_EVIDENCE_RE.search(combined)) or bool(stalls)

    if first_error and not frame_evidence:
        verdict = "fail"
        summary = "Fails to launch / crashes before ever rendering a frame."
    elif frame_evidence and severe == 0 and not first_error:
        verdict = "ok"
        summary = "Playable — no severe stalls or error markers detected."
    elif frame_evidence and (severe > 0 or first_error):
        worst = max(stalls) if stalls else 0
        bits = [f"{severe} severe stall(s), worst {worst}ms"]
        if first_error:
            bits.append("at least one error/crash marker logged")
        verdict = "issues"
        summary = "Runs, but with issues (" + "; ".join(bits) + ")."
    else:
        verdict = "unknown"
        summary = "Inconclusive — logs didn't match known success/failure patterns; needs manual review."

    return {
        "verdict": verdict,
        "summary": summary,
        "stall_count": len(stalls),
        "severe_count": severe,
        "max_stall_ms": max(stalls) if stalls else 0,
        "avg_stall_ms": round(sum(stalls) / len(stalls), 1) if stalls else 0,
        "first_error": first_error,
    }


def format_env_line(meta):
    build = meta.get("engine_build") or "Unknown (submitted before build/firmware logging existed)"
    firmware = meta.get("switch_firmware") or "Unknown"
    atmosphere = meta.get("atmosphere_version") or "Unknown"
    return f"**Test environment:** Android Horizon build `{build}`, Switch firmware `{firmware}`, Atmosphere `{atmosphere}`  "


APK_INTEGRITY_LABEL = {
    "match":      "✅ Matches — logs came from this exact APK",
    "mismatch":   "⚠️ MISMATCH — logs were generated from a different APK than the one submitted here",
    "not_logged": "❓ Not available (submitted before this check existed, or the engine build predates it)",
}


def format_apk_integrity_line(meta):
    check = meta.get("apk_integrity_check") or "not_logged"
    return f"**APK integrity check:** {APK_INTEGRITY_LABEL[check]}  "


def build_report_md(meta, analysis, old_meta):
    lines = [
        f"# {meta['game_name']} — {meta['version_name']}", "",
        f"**Package:** `{meta['package_name']}`  ",
        f"**Verdict:** {VERDICT_LABEL[analysis['verdict']]}  ",
        f"**Source:** {meta['source_site']} — {meta['apk_url']}  ",
        f"**APK SHA-256:** `{meta['apk_sha256']}`  ",
        format_apk_integrity_line(meta),
        f"**Play Store category check:** {meta['play_store_category_check']}  ",
        f"**Submitted by:** {meta['submitted_by']}  ",
        format_env_line(meta),
    ]
    if old_meta:
        lines.append(f"**Supersedes:** submission by {old_meta.get('submitted_by')} on {old_meta.get('submitted_at')}  ")
    lines += ["", "## Analysis", "", analysis["summary"], "",
              f"- Frame stalls logged: **{analysis['stall_count']}** (severe: **{analysis['severe_count']}**)",
              f"- Worst stall: **{analysis['max_stall_ms']}ms**, average: **{analysis['avg_stall_ms']}ms**"]
    if analysis["first_error"]:
        lines.append(f"- First error/crash line found: `{analysis['first_error']}`")
    lines.append("")
    if meta.get("notes"):
        lines += ["## Submitter notes", "", meta["notes"], ""]
    lines += ["## Raw logs", "", "See `launcher_log.txt`, `compat_log.txt`, `core_log.txt` in this folder.", ""]
    return "\n".join(lines)


def commit_and_push(reports_dir, message):
    subprocess.run(["git", "-C", reports_dir, "config", "user.name", "android-horizon-bot"], check=True)
    subprocess.run(["git", "-C", reports_dir, "config", "user.email",
                    "github-actions[bot]@users.noreply.github.com"], check=True)
    subprocess.run(["git", "-C", reports_dir, "add", "-A"], check=True)
    result = subprocess.run(["git", "-C", reports_dir, "commit", "-m", message], capture_output=True, text=True)
    if result.returncode != 0 and "nothing to commit" not in (result.stdout + result.stderr):
        raise RuntimeError(f"git commit failed: {result.stderr}")
    subprocess.run(["git", "-C", reports_dir, "push", "origin", "main"], check=True)


# -------------------------------------------------------------------- main --

def main():
    submission_id = os.environ["SUBMISSION_ID"]
    org_pat = os.environ.get("ORG_PAT", "").strip()

    if not org_pat:
        print("::error::ORG_PAT secret isn't set — can't read/write compat-reports. "
              "Add a classic PAT with repo scope on the org as the ORG_PAT secret.")
        sys.exit(1)

    workdir = tempfile.mkdtemp()
    reports_dir = os.path.join(workdir, "compat-reports")
    clone_url = f"https://{org_pat}@github.com/AndroidHorizon/compat-reports.git"
    subprocess.run(["git", "clone", "--depth", "1", clone_url, reports_dir], check=True, capture_output=True)

    pending_dir = os.path.join(reports_dir, "pending", submission_id)
    if not os.path.isdir(pending_dir):
        print(f"::error::pending submission {submission_id} not found in compat-reports")
        sys.exit(1)

    def fail(reason):
        with open(os.path.join(pending_dir, "error.txt"), "w") as f:
            f.write(reason + "\n")
        try:
            commit_and_push(reports_dir, f"Failed submission {submission_id}: {reason[:72]}")
        except Exception as e:
            print(f"::warning::couldn't even commit the error note: {e}")
        print(f"::error::{reason}")
        sys.exit(1)

    try:
        with open(os.path.join(pending_dir, "meta.json")) as f:
            meta_in = json.load(f)
    except Exception as e:
        fail(f"couldn't read meta.json for {submission_id} ({e})")
        return

    # Whether this was a pasted link or a directly-attached file, the Worker
    # always resolves it to a plain https URL by the time it lands here — a
    # presigned R2 GET URL for uploads (valid a few hours, long enough for
    # the Actions queue), or the submitter's own link otherwise. Either way
    # this script just downloads a URL; it doesn't need to know which.
    apk_url = (meta_in.get("apk_url") or "").strip()
    source_site = (meta_in.get("source_site") or "").strip()
    github_username = (meta_in.get("github_username") or "").strip()
    notes = (meta_in.get("notes") or "").strip()

    def read_log(name):
        p = os.path.join(pending_dir, name)
        if not os.path.exists(p):
            return ""
        with open(p, "r", errors="replace") as f:
            return f.read()

    launcher_log = read_log("launcher_log.txt")
    compat_log = read_log("compat_log.txt")
    core_log = read_log("core_log.txt")

    if not apk_url or not source_site or not (launcher_log and compat_log and core_log):
        fail("missing required fields (apk_url / source_site / one or more logs)")
        return

    parsed = urllib.parse.urlparse(apk_url)
    if parsed.scheme not in ("http", "https"):
        fail(f"apk_url isn't a valid http(s) link: {apk_url}")
        return

    apk_path = os.path.join(workdir, "game.apk")
    ok, http_code, err = download_apk(apk_url, apk_path)
    if not ok:
        fail(f"couldn't download the APK ({err or 'unknown error'})")
        return

    with open(apk_path, "rb") as f:
        apk_bytes = f.read()
    if apk_bytes[:2] != b"PK":
        fail("the APK isn't a valid ZIP (bad magic bytes)")
        return
    apk_sha256 = hashlib.sha256(apk_bytes).hexdigest()
    del apk_bytes

    package, version_name, app_name, meta_err = read_apk_metadata(apk_path)
    if meta_err:
        fail(f"couldn't read the APK's manifest ({meta_err})")
        return

    icon_path = extract_icon(apk_path, os.path.join(workdir, "icon"))
    play_status = check_play_store_category(package)
    if play_status == "NOT_GAME":
        fail(f"Play Store lists {package} as a non-game app, not a game")
        return

    analysis = analyze_logs(launcher_log, compat_log, core_log)
    env_info = parse_env_header(compat_log)
    apk_integrity_check, logged_apk_sha256 = check_apk_integrity(compat_log, apk_sha256)

    pkg = safe_path_component(package)
    ver = safe_path_component(version_name)
    target = os.path.join(reports_dir, "reports", pkg, ver)

    old_meta = None
    if os.path.isdir(target):
        try:
            with open(os.path.join(target, "meta.json")) as f:
                old_meta = json.load(f)
        except Exception:
            old_meta = None
        shutil.rmtree(target)
    os.makedirs(target, exist_ok=True)

    with open(os.path.join(target, "launcher_log.txt"), "w") as f:
        f.write(launcher_log)
    with open(os.path.join(target, "compat_log.txt"), "w") as f:
        f.write(compat_log)
    with open(os.path.join(target, "core_log.txt"), "w") as f:
        f.write(core_log)

    icon_rel = None
    if icon_path and os.path.exists(icon_path):
        icon_rel = "icon" + os.path.splitext(icon_path)[1]
        shutil.copy(icon_path, os.path.join(target, icon_rel))

    # A directly-attached upload resolves to a presigned R2 URL, which is
    # long, ugly, and dead within hours — not worth keeping in a report
    # that's meant to be readable indefinitely.
    apk_url_display = "(uploaded directly, no link)" if "r2.cloudflarestorage.com" in apk_url else apk_url

    now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    meta = {
        "game_name": app_name, "package_name": pkg, "version_name": ver,
        "apk_url": apk_url_display,
        "apk_sha256": apk_sha256, "source_site": source_site,
        "submitted_by": github_username or "anonymous", "submitted_at": now,
        "notes": notes, "play_store_category_check": play_status,
        "superseded_submission_by": old_meta.get("submitted_by") if old_meta else None,
        **env_info,
        "apk_integrity_check": apk_integrity_check,
    }
    with open(os.path.join(target, "meta.json"), "w") as f:
        json.dump(meta, f, indent=2)
    with open(os.path.join(target, "report.md"), "w") as f:
        f.write(build_report_md(meta, analysis, old_meta))

    games_json_path = os.path.join(reports_dir, "data", "games.json")
    try:
        with open(games_json_path) as f:
            games = json.load(f)
    except Exception:
        games = []
    games = [g for g in games if g.get("package_name") != pkg]
    games.append({
        "game_name": app_name, "package_name": pkg, "version_name": ver,
        "verdict": analysis["verdict"], "verdict_label": VERDICT_LABEL[analysis["verdict"]],
        "summary": analysis["summary"],
        "icon": f"reports/{pkg}/{ver}/{icon_rel}" if icon_rel else None,
        "source_site": source_site,
        "submitted_by": github_username if github_username else "",
        "submitted_at": now, "report_path": f"reports/{pkg}/{ver}/report.md",
    })
    games.sort(key=lambda g: g["game_name"].lower())
    with open(games_json_path, "w") as f:
        json.dump(games, f, indent=2)

    shutil.rmtree(pending_dir)

    commit_msg = (f"{'Update' if old_meta else 'Add'} {app_name} {ver} "
                  f"({VERDICT_LABEL[analysis['verdict']]})\n\nSubmitted via website form ({submission_id})")
    commit_and_push(reports_dir, commit_msg)
    print(f"Published {app_name} {ver}: {VERDICT_LABEL[analysis['verdict']]}")


if __name__ == "__main__":
    main()
