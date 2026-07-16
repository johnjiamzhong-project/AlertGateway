#!/usr/bin/env python3
import argparse
import csv
import html
import json
import shutil
from pathlib import Path


DEFAULT_RAW_ROOT = Path("/home/rambos/datasets/raw_extracted")
DEFAULT_OUTPUT_DIR = Path("/home/rambos/datasets/selected_desktop6/review")
DEFAULT_ASSET_DIR_NAME = "_frames"


SESSION_NOTES = {
    "collect_001_all_objects": "mixed objects; keep only clear frames, discard UI/blurred frames",
    "collect_002_keyboard_mouse_phone": "keyboard, mouse, cell phone",
    "collect_003_cup_book_phone": "cup variants, book, cell phone",
    "collect_004_keyboard_mouse_cup": "keyboard, mouse, cup variants; discard blurred motion frames",
    "collect_005_laptop_monitor_2s": "monitor as laptop plus keyboard/mouse; keep only clear target frames",
    "collect_006_laptop_monitor_clean_2s": "clean monitor frames; label monitor as laptop",
    "collect_007_laptop_open_occluded_2s": "open laptop; discard frames without laptop body",
    "collect_008_cup_variants_2s": "cup variants; discard heavy motion blur",
    "collect_009_book_variants_2s": "book variants; discard blurred motion frames",
    "collect_010_clutter_negative_2s": "negative clutter; keep only frames without six target classes",
    "collect_011_empty_negative_2s": "empty-table negative samples",
}


def iter_images(raw_root: Path):
    for session_dir in sorted(p for p in raw_root.iterdir() if p.is_dir()):
        images = sorted(session_dir.glob("*.jpg"))
        if not images:
            continue
        note = SESSION_NOTES.get(session_dir.name, "")
        for image in images:
            yield session_dir.name, image, note


def write_csv(rows, csv_path: Path):
    with csv_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["keep", "session", "image_path", "suggested_labels", "note"])
        for session, image, note in rows:
            writer.writerow(["", session, str(image), "", note])


def asset_name(session: str, image: Path) -> str:
    return f"{session}__{image.name}"


def copy_assets(rows, asset_dir: Path):
    asset_dir.mkdir(parents=True, exist_ok=True)
    copied = 0
    reused = 0
    for session, image, _note in rows:
        destination = asset_dir / asset_name(session, image)
        if destination.exists() and destination.stat().st_size == image.stat().st_size:
            reused += 1
            continue
        shutil.copy2(image, destination)
        copied += 1
    return copied, reused


def write_html(rows, html_path: Path, columns: int, asset_dir_name: str):
    style = """
body { font-family: sans-serif; margin: 24px; background: #f7f7f7; color: #222; }
h1 { font-size: 22px; margin: 0; }
h2 { font-size: 18px; margin-top: 28px; border-bottom: 1px solid #ccc; padding-bottom: 6px; }
.toolbar { position: sticky; top: 0; z-index: 10; background: #f7f7f7; border-bottom: 1px solid #ddd; padding: 0 0 12px; }
.actions { display: flex; flex-wrap: wrap; gap: 8px; margin-top: 12px; align-items: center; }
.status { color: #444; font-size: 13px; margin-left: 8px; }
.grid { display: grid; grid-template-columns: repeat(COLS, minmax(0, 1fr)); gap: 12px; }
.card { background: white; border: 1px solid #ddd; padding: 8px; }
.card img { width: 100%; height: auto; display: block; }
.meta { font-size: 12px; word-break: break-all; margin-top: 6px; color: #555; }
.note { font-size: 13px; color: #333; margin-bottom: 10px; }
button { border: 1px solid #999; background: #fff; padding: 6px 10px; cursor: pointer; }
button:hover { background: #eee; }
.skip { border-color: #8a3a3a; color: #8a3a3a; }
.restore { border-color: #177245; color: #177245; }
.card-actions { display: flex; gap: 8px; margin-top: 8px; }
.hidden { display: none; }
.deleted { opacity: 0.42; }
""".replace("COLS", str(columns))

    sessions = {}
    for session, image, note in rows:
        sessions.setdefault(session, {"note": note, "images": []})["images"].append(image)

    row_data = [
        {
            "session": session,
            "image_path": str(image),
            "asset_path": f"{asset_dir_name}/{asset_name(session, image)}",
            "image_name": image.name,
            "suggested_labels": "",
            "note": note,
        }
        for session, image, note in rows
    ]

    parts = [
        "<!doctype html>",
        "<meta charset='utf-8'>",
        "<title>AlertGateway desktop6 review index</title>",
        f"<style>{style}</style>",
        "<div class='toolbar'>",
        "<h1>AlertGateway desktop6 frame review</h1>",
        "<div class='actions'>",
        "<button id='exportKeep'>导出保留CSV</button>",
        "<button id='exportAll'>导出全部记录CSV</button>",
        "<button id='showRemaining'>只看保留候选</button>",
        "<button id='showAll'>显示全部</button>",
        "<button id='resetChoices'>恢复全部默认保留</button>",
        "<span class='status' id='status'></span>",
        "</div>",
        "</div>",
    ]
    for session, data in sessions.items():
        parts.append(f"<h2>{html.escape(session)} ({len(data['images'])} frames)</h2>")
        if data["note"]:
            parts.append(f"<div class='note'>{html.escape(data['note'])}</div>")
        parts.append("<div class='grid'>")
        for image in data["images"]:
            key = str(image)
            src = f"{asset_dir_name}/{asset_name(session, image)}"
            parts.append(f"<div class='card' data-key='{html.escape(key)}'>")
            parts.append(f"<img src='{html.escape(src)}' loading='lazy'>")
            parts.append(f"<div class='meta'>{html.escape(image.name)}</div>")
            parts.append("<div class='card-actions'>")
            parts.append("<button class='skip' data-choice='skip'>删除</button>")
            parts.append("<button class='restore' data-choice='restore'>恢复</button>")
            parts.append("</div>")
            parts.append("</div>")
        parts.append("</div>")

    script = f"""
<script>
const rows = {json.dumps(row_data, ensure_ascii=False)};
const storageKey = 'alertgateway_desktop6_review_choices_v2';
const choices = new Map(Object.entries(JSON.parse(localStorage.getItem(storageKey) || '{{}}')));
let hideProcessed = true;

function saveChoices() {{
  localStorage.setItem(storageKey, JSON.stringify(Object.fromEntries(choices)));
}}

function updateStatus() {{
  let skip = 0;
  for (const choice of choices.values()) {{
    if (choice === 'skip') skip++;
  }}
  document.getElementById('status').textContent = `默认保留 ${{rows.length - skip}}，已删除 ${{skip}}，总数 ${{rows.length}}`;
}}

function applyVisibility() {{
  for (const card of document.querySelectorAll('.card')) {{
    const key = card.dataset.key;
    const deleted = choices.get(key) === 'skip';
    card.classList.toggle('deleted', deleted);
    card.classList.toggle('hidden', hideProcessed && deleted);
  }}
  updateStatus();
}}

function choose(card, choice) {{
  if (choice === 'restore') {{
    choices.delete(card.dataset.key);
  }} else {{
    choices.set(card.dataset.key, choice);
  }}
  saveChoices();
  applyVisibility();
}}

function csvEscape(value) {{
  const text = String(value ?? '');
  if (/[",\\n\\r]/.test(text)) {{
    return '"' + text.replaceAll('"', '""') + '"';
  }}
  return text;
}}

function exportCsv(keepOnly) {{
  const header = ['keep', 'review_choice', 'session', 'image_path', 'suggested_labels', 'note'];
  const lines = [header.join(',')];
  for (const row of rows) {{
    const choice = choices.get(row.image_path) || 'keep';
    if (keepOnly && choice === 'skip') continue;
    lines.push([
      choice === 'keep' ? '1' : '',
      choice,
      row.session,
      row.image_path,
      row.suggested_labels,
      row.note,
    ].map(csvEscape).join(','));
  }}
  const blob = new Blob([lines.join('\\n') + '\\n'], {{ type: 'text/csv;charset=utf-8' }});
  const link = document.createElement('a');
  link.href = URL.createObjectURL(blob);
  link.download = keepOnly ? 'review_keep_selection.csv' : 'review_all_choices.csv';
  link.click();
  URL.revokeObjectURL(link.href);
}}

document.addEventListener('click', event => {{
  const choice = event.target.dataset.choice;
  if (choice) {{
    choose(event.target.closest('.card'), choice);
  }}
}});

document.getElementById('exportKeep').addEventListener('click', () => exportCsv(true));
document.getElementById('exportAll').addEventListener('click', () => exportCsv(false));
document.getElementById('showRemaining').addEventListener('click', () => {{
  hideProcessed = true;
  applyVisibility();
}});
document.getElementById('showAll').addEventListener('click', () => {{
  hideProcessed = false;
  applyVisibility();
}});
document.getElementById('resetChoices').addEventListener('click', () => {{
  if (confirm('恢复全部图片为默认保留？')) {{
    choices.clear();
    saveChoices();
    applyVisibility();
  }}
}});

applyVisibility();
</script>
"""
    parts.append(script)

    html_path.write_text("\n".join(parts) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(
        description="Build a manual review manifest and HTML index for extracted desktop6 frames."
    )
    parser.add_argument("--raw-root", type=Path, default=DEFAULT_RAW_ROOT)
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT_DIR)
    parser.add_argument("--columns", type=int, default=4)
    parser.add_argument("--asset-dir-name", default=DEFAULT_ASSET_DIR_NAME)
    args = parser.parse_args()

    rows = list(iter_images(args.raw_root))
    args.output_dir.mkdir(parents=True, exist_ok=True)
    copied, reused = copy_assets(rows, args.output_dir / args.asset_dir_name)

    csv_path = args.output_dir / "review_manifest.csv"
    html_path = args.output_dir / "review_index.html"
    write_csv(rows, csv_path)
    write_html(rows, html_path, max(1, args.columns), args.asset_dir_name)

    print(f"frames: {len(rows)}")
    print(f"assets_copied: {copied}")
    print(f"assets_reused: {reused}")
    print(f"csv: {csv_path}")
    print(f"html: {html_path}")


if __name__ == "__main__":
    main()
