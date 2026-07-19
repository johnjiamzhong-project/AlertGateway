#!/usr/bin/env python3
import argparse
import json
import mimetypes
import shutil
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote


DEFAULT_ANNOTATION_DIR = Path("/home/rambos/datasets/alertgateway_desktop6/annotation")
DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 8765


INDEX_HTML = r"""<!doctype html>
<meta charset="utf-8">
<title>AlertGateway YOLO Annotator</title>
<style>
body { margin: 0; font-family: Arial, sans-serif; color: #1f2933; background: #f4f5f7; }
.app { display: grid; grid-template-columns: 1fr 300px; height: 100vh; }
.stage { display: flex; align-items: center; justify-content: center; overflow: auto; padding: 16px; }
.canvas-wrap { position: relative; background: #111; line-height: 0; }
canvas { display: block; cursor: crosshair; }
.side { border-left: 1px solid #d7dce2; background: #fff; padding: 14px; overflow: auto; }
h1 { font-size: 18px; margin: 0 0 10px; }
.row { display: flex; flex-wrap: wrap; gap: 8px; margin: 10px 0; align-items: center; }
button { border: 1px solid #a9b2bd; background: #fff; padding: 7px 10px; cursor: pointer; border-radius: 4px; }
button:hover { background: #f0f2f5; }
button.primary { border-color: #1d6fb8; color: #0b5a9d; }
button.danger { border-color: #a33b3b; color: #8a2424; }
button.active { background: #dbeafe; border-color: #2563eb; }
.meta { font-size: 12px; color: #52606d; word-break: break-all; margin: 8px 0; }
.status { font-size: 13px; background: #f6f8fa; border: 1px solid #d7dce2; padding: 8px; margin: 8px 0; }
.classes button { width: 100%; text-align: left; }
.boxes { font-size: 13px; margin-top: 10px; }
.box-item { display: flex; justify-content: space-between; gap: 8px; border-bottom: 1px solid #eee; padding: 6px 0; }
.hint { font-size: 12px; color: #52606d; line-height: 1.4; }
</style>
<div class="app">
  <main class="stage">
    <div class="canvas-wrap">
      <canvas id="canvas"></canvas>
    </div>
  </main>
  <aside class="side">
    <h1>YOLO 标注</h1>
    <div class="status" id="status">loading...</div>
    <div class="meta" id="imageName"></div>
    <div class="row">
      <button id="prev">上一张</button>
      <button id="next">下一张</button>
    </div>
    <div class="row">
      <label for="jumpIndex" class="hint" style="margin-right: 4px;">跳转到</label>
      <input id="jumpIndex" type="number" min="1" step="1" style="width: 84px; padding: 6px 8px;">
      <button id="jump">跳转</button>
    </div>
    <div class="row">
      <button class="primary" id="save">保存</button>
      <button class="danger" id="empty">标为空负样本</button>
      <button class="danger" id="deleteImage">删除图片</button>
    </div>
    <div class="row">
      <button id="deleteBox">删除选中框</button>
      <button id="clearBoxes">清空本图框</button>
    </div>
    <div class="row">
      <button id="zoomOut">缩小</button>
      <button id="zoomIn">放大</button>
      <button id="zoomFit">适应窗口</button>
    </div>
    <div class="classes" id="classes"></div>
    <div class="boxes" id="boxes"></div>
    <p class="hint">
      操作：空白处拖动新建框；点击已有框选中后，可拖框内移动，或拖黄色控制点调整边和角。
      放大后可用画面区域的滚动条查看细节。点类别按钮或按 0-5 可修改选中框类别。
      没有选中框时，类别按钮只决定下一次新画框类别。删除选中框可移除。
      每张图完成后点保存。显示器按 laptop 标，负样本确认无六类目标后点标为空负样本。
    </p>
  </aside>
</div>
<script>
const canvas = document.getElementById('canvas');
const ctx = canvas.getContext('2d');
const statusEl = document.getElementById('status');
const imageNameEl = document.getElementById('imageName');
const classesEl = document.getElementById('classes');
const boxesEl = document.getElementById('boxes');

let images = [];
let classes = [];
let index = 0;
let image = new Image();
let boxes = [];
let selectedClass = 0;
let selectedBox = -1;
let drawing = null;
let pointerStart = null;
let scale = 1;
let zoom = 1;
const dragThreshold = 4;
const handleRadius = 7;
const jumpIndexEl = document.getElementById('jumpIndex');

function setStatus(text) { statusEl.textContent = text; }

function fitCanvas() {
  const maxW = Math.max(360, window.innerWidth - 340);
  const maxH = Math.max(260, window.innerHeight - 40);
  scale = Math.min(maxW / image.naturalWidth, maxH / image.naturalHeight, 1.4) * zoom;
  canvas.width = Math.round(image.naturalWidth * scale);
  canvas.height = Math.round(image.naturalHeight * scale);
}

function toCanvasBox(box) {
  const w = box.w * canvas.width;
  const h = box.h * canvas.height;
  return {
    x: box.x * canvas.width - w / 2,
    y: box.y * canvas.height - h / 2,
    w, h,
  };
}

function fromCanvasRect(rect, cls) {
  const x1 = Math.max(0, Math.min(rect.x1, rect.x2));
  const y1 = Math.max(0, Math.min(rect.y1, rect.y2));
  const x2 = Math.min(canvas.width, Math.max(rect.x1, rect.x2));
  const y2 = Math.min(canvas.height, Math.max(rect.y1, rect.y2));
  const w = x2 - x1;
  const h = y2 - y1;
  if (w < 5 || h < 5) return null;
  return {
    cls,
    x: (x1 + w / 2) / canvas.width,
    y: (y1 + h / 2) / canvas.height,
    w: w / canvas.width,
    h: h / canvas.height,
  };
}

function draw() {
  if (!image.complete) return;
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  ctx.drawImage(image, 0, 0, canvas.width, canvas.height);
  boxes.forEach((box, i) => {
    const r = toCanvasBox(box);
    ctx.lineWidth = i === selectedBox ? 3 : 2;
    ctx.strokeStyle = i === selectedBox ? '#ffdf2b' : '#22c55e';
    ctx.fillStyle = 'rgba(0,0,0,0.65)';
    ctx.strokeRect(r.x, r.y, r.w, r.h);
    const label = `${box.cls} ${classes[box.cls]}`;
    ctx.font = '14px Arial';
    const textW = ctx.measureText(label).width + 8;
    ctx.fillRect(r.x, Math.max(0, r.y - 20), textW, 20);
    ctx.fillStyle = '#fff';
    ctx.fillText(label, r.x + 4, Math.max(14, r.y - 6));
    if (i === selectedBox) drawResizeHandles(r);
  });
  if (drawing) {
    ctx.lineWidth = 2;
    ctx.strokeStyle = '#38bdf8';
    ctx.strokeRect(drawing.x1, drawing.y1, drawing.x2 - drawing.x1, drawing.y2 - drawing.y1);
  }
  renderBoxes();
}

function drawResizeHandles(rect) {
  const handles = resizeHandles(rect);
  ctx.fillStyle = '#ffdf2b';
  ctx.strokeStyle = '#1f2933';
  ctx.lineWidth = 1;
  Object.values(handles).forEach(point => {
    ctx.beginPath();
    ctx.rect(point.x - handleRadius, point.y - handleRadius, handleRadius * 2, handleRadius * 2);
    ctx.fill();
    ctx.stroke();
  });
}

function renderClasses() {
  classesEl.innerHTML = '';
  classes.forEach((name, i) => {
    const btn = document.createElement('button');
    btn.textContent = `${i} ${name}`;
    btn.className = i === selectedClass ? 'active' : '';
    btn.onclick = () => applyClass(i);
    classesEl.appendChild(btn);
  });
}

function applyClass(cls) {
  if (selectedBox < 0 && boxes.length === 1) selectedBox = 0;
  selectedClass = cls;
  if (selectedBox >= 0 && selectedBox < boxes.length) {
    boxes[selectedBox].cls = cls;
    setStatus(`${index + 1}/${images.length} 已将选中框改为 ${classes[cls]}，请点击保存`);
  } else if (boxes.length > 0) {
    setStatus(`${index + 1}/${images.length} 请先点击要修改的框；当前 ${classes[cls]} 只用于新建框`);
  }
  renderClasses();
  draw();
}

function renderBoxes() {
  boxesEl.innerHTML = `<strong>本图框：${boxes.length}</strong>`;
  boxes.forEach((box, i) => {
    const div = document.createElement('div');
    div.className = 'box-item';
    div.innerHTML = `<span>${i + 1}. ${classes[box.cls]}</span><button>${i === selectedBox ? '已选中' : '选中'}</button>`;
    div.querySelector('button').onclick = () => {
      selectedBox = i;
      selectedClass = boxes[i].cls;
      renderClasses();
      draw();
    };
    boxesEl.appendChild(div);
  });
}

async function loadCurrent() {
  const item = images[index];
  imageNameEl.textContent = item.name;
  jumpIndexEl.max = String(images.length);
  jumpIndexEl.value = String(index + 1);
  setStatus(`${index + 1}/${images.length} ${item.labeled ? '已有标签' : '未标注'}${item.negative_hint ? ' | 负样本候选' : ''}`);
  image = new Image();
  image.onload = async () => {
    fitCanvas();
    const labelResp = await fetch(`/api/labels/${encodeURIComponent(item.name)}`);
    boxes = await labelResp.json();
    selectedBox = -1;
    draw();
  };
  image.src = `/image/${encodeURIComponent(item.name)}`;
}

async function saveCurrent(empty=false) {
  const item = images[index];
  const payload = empty ? [] : boxes;
  const resp = await fetch(`/api/labels/${encodeURIComponent(item.name)}`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(payload),
  });
  if (!resp.ok) {
    setStatus('保存失败');
    return;
  }
  item.labeled = true;
  if (empty) boxes = [];
  setStatus(`${index + 1}/${images.length} 已保存`);
  draw();
}

async function deleteCurrentImage() {
  const item = images[index];
  if (!confirm(`删除这张图片并移出标注集？\n${item.name}`)) return;
  const resp = await fetch(`/api/delete/${encodeURIComponent(item.name)}`, { method: 'POST' });
  if (!resp.ok) {
    setStatus('删除失败');
    return;
  }
  images.splice(index, 1);
  if (images.length === 0) {
    boxes = [];
    image = new Image();
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    imageNameEl.textContent = '';
    boxesEl.innerHTML = '';
    setStatus('已删除所有图片');
    return;
  }
  if (index >= images.length) {
    index = images.length - 1;
  }
  loadCurrent();
}

function jumpToImage(targetIndex) {
  if (!Number.isInteger(targetIndex)) return;
  if (targetIndex < 0 || targetIndex >= images.length) return;
  index = targetIndex;
  loadCurrent();
}

function canvasPoint(event) {
  const rect = canvas.getBoundingClientRect();
  return { x: event.clientX - rect.left, y: event.clientY - rect.top };
}

function clampCanvasPoint(point) {
  return {
    x: Math.max(0, Math.min(canvas.width, point.x)),
    y: Math.max(0, Math.min(canvas.height, point.y)),
  };
}

function hitTest(point) {
  for (let i = boxes.length - 1; i >= 0; i--) {
    const r = toCanvasBox(boxes[i]);
    if (point.x >= r.x && point.x <= r.x + r.w && point.y >= r.y && point.y <= r.y + r.h) return i;
  }
  return -1;
}

function resizeHandles(rect) {
  const cx = rect.x + rect.w / 2;
  const cy = rect.y + rect.h / 2;
  return {
    nw: { x: rect.x, y: rect.y }, n: { x: cx, y: rect.y }, ne: { x: rect.x + rect.w, y: rect.y },
    e: { x: rect.x + rect.w, y: cy }, se: { x: rect.x + rect.w, y: rect.y + rect.h },
    s: { x: cx, y: rect.y + rect.h }, sw: { x: rect.x, y: rect.y + rect.h }, w: { x: rect.x, y: cy },
  };
}

function selectedEditHit(point) {
  if (selectedBox < 0 || selectedBox >= boxes.length) return null;
  const rect = toCanvasBox(boxes[selectedBox]);
  for (const [mode, handle] of Object.entries(resizeHandles(rect))) {
    if (Math.hypot(point.x - handle.x, point.y - handle.y) <= handleRadius + 3) {
      return { mode, rect };
    }
  }
  if (point.x >= rect.x && point.x <= rect.x + rect.w && point.y >= rect.y && point.y <= rect.y + rect.h) {
    return { mode: 'move', rect };
  }
  return null;
}

function clampRect(rect) {
  let { x1, y1, x2, y2 } = rect;
  x1 = Math.max(0, Math.min(canvas.width - 5, x1));
  y1 = Math.max(0, Math.min(canvas.height - 5, y1));
  x2 = Math.max(5, Math.min(canvas.width, x2));
  y2 = Math.max(5, Math.min(canvas.height, y2));
  if (x2 - x1 < 5) x2 = Math.min(canvas.width, x1 + 5);
  if (y2 - y1 < 5) y2 = Math.min(canvas.height, y1 + 5);
  return { x1, y1, x2, y2 };
}

function editedRect(original, mode, point, start) {
  let x1 = original.x, y1 = original.y, x2 = original.x + original.w, y2 = original.y + original.h;
  if (mode === 'move') {
    const dx = point.x - start.x, dy = point.y - start.y;
    x1 += dx; x2 += dx; y1 += dy; y2 += dy;
    if (x1 < 0) { x2 -= x1; x1 = 0; }
    if (y1 < 0) { y2 -= y1; y1 = 0; }
    if (x2 > canvas.width) { x1 -= x2 - canvas.width; x2 = canvas.width; }
    if (y2 > canvas.height) { y1 -= y2 - canvas.height; y2 = canvas.height; }
    return { x1, y1, x2, y2 };
  }
  if (mode.includes('w')) x1 = point.x;
  if (mode.includes('e')) x2 = point.x;
  if (mode.includes('n')) y1 = point.y;
  if (mode.includes('s')) y2 = point.y;
  return clampRect({ x1, y1, x2, y2 });
}

function setSelectedBoxFromRect(rect) {
  const box = fromCanvasRect(rect, boxes[selectedBox].cls);
  if (box) boxes[selectedBox] = box;
}

canvas.addEventListener('mousedown', event => {
  const p = canvasPoint(event);
  const edit = selectedEditHit(p);
  pointerStart = { x: p.x, y: p.y, hit: hitTest(p), edit };
  drawing = null;
  if (edit) canvas.style.cursor = edit.mode === 'move' ? 'move' : 'nwse-resize';
});
window.addEventListener('mousemove', event => {
  if (!pointerStart) return;
  const p = clampCanvasPoint(canvasPoint(event));
  if (pointerStart.edit) {
    setSelectedBoxFromRect(editedRect(pointerStart.edit.rect, pointerStart.edit.mode, p, pointerStart));
    draw();
    return;
  }
  const dx = p.x - pointerStart.x;
  const dy = p.y - pointerStart.y;
  if (!drawing && Math.hypot(dx, dy) >= dragThreshold) {
    drawing = { x1: pointerStart.x, y1: pointerStart.y, x2: p.x, y2: p.y };
    selectedBox = -1;
  }
  if (!drawing) return;
  drawing.x2 = p.x;
  drawing.y2 = p.y;
  draw();
});
window.addEventListener('mouseup', event => {
  if (!pointerStart) return;
  if (pointerStart.edit) {
    const p = clampCanvasPoint(canvasPoint(event));
    setSelectedBoxFromRect(editedRect(pointerStart.edit.rect, pointerStart.edit.mode, p, pointerStart));
    pointerStart = null;
    canvas.style.cursor = 'crosshair';
    draw();
    return;
  }
  if (!drawing) {
    if (pointerStart.hit >= 0) {
      selectedBox = pointerStart.hit;
      selectedClass = boxes[pointerStart.hit].cls;
      renderClasses();
      draw();
    }
    pointerStart = null;
    return;
  }
  const p = clampCanvasPoint(canvasPoint(event));
  drawing.x2 = p.x;
  drawing.y2 = p.y;
  const box = fromCanvasRect(drawing, selectedClass);
  drawing = null;
  pointerStart = null;
  if (box) boxes.push(box);
  selectedBox = boxes.length - 1;
  draw();
});

document.getElementById('prev').onclick = () => { if (index > 0) { index--; loadCurrent(); } };
document.getElementById('next').onclick = () => { if (index + 1 < images.length) { index++; loadCurrent(); } };
document.getElementById('jump').onclick = () => {
  const value = Number.parseInt(jumpIndexEl.value, 10);
  if (!Number.isFinite(value)) return;
  jumpToImage(value - 1);
};
document.getElementById('save').onclick = () => saveCurrent(false);
document.getElementById('empty').onclick = () => {
  if (confirm('确认这张图没有六类目标，并保存为空标签？')) saveCurrent(true);
};
document.getElementById('deleteImage').onclick = () => deleteCurrentImage();
document.getElementById('deleteBox').onclick = () => {
  if (selectedBox >= 0) boxes.splice(selectedBox, 1);
  selectedBox = -1;
  draw();
};
document.getElementById('clearBoxes').onclick = () => {
  if (confirm('清空本图所有框？')) { boxes = []; selectedBox = -1; draw(); }
};
document.getElementById('zoomOut').onclick = () => { zoom = Math.max(0.5, zoom / 1.25); fitCanvas(); draw(); };
document.getElementById('zoomIn').onclick = () => { zoom = Math.min(4, zoom * 1.25); fitCanvas(); draw(); };
document.getElementById('zoomFit').onclick = () => { zoom = 1; fitCanvas(); draw(); };
window.addEventListener('resize', () => { fitCanvas(); draw(); });
window.addEventListener('keydown', event => {
  if (event.key >= '0' && event.key <= '5') applyClass(Number(event.key));
  if (event.key === 's') saveCurrent(false);
  if (event.key === 'ArrowRight') document.getElementById('next').click();
  if (event.key === 'ArrowLeft') document.getElementById('prev').click();
  if (event.key === 'Delete') document.getElementById('deleteBox').click();
  if (event.key === 'D' && event.shiftKey) deleteCurrentImage();
  if (event.key === 'Enter' && document.activeElement === jumpIndexEl) document.getElementById('jump').click();
});

async function main() {
  const resp = await fetch('/api/images');
  const data = await resp.json();
  images = data.images;
  classes = data.classes;
  renderClasses();
  loadCurrent();
}
main();
</script>
"""


class AnnotatorHandler(BaseHTTPRequestHandler):
    server_version = "AlertGatewayYoloAnnotator/1.0"

    @property
    def app(self):
        return self.server.app

    def send_bytes(self, data: bytes, content_type: str, status: int = 200):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def send_json(self, data, status: int = 200):
        self.send_bytes(json.dumps(data, ensure_ascii=False).encode("utf-8"), "application/json; charset=utf-8", status)

    def do_GET(self):
        path = unquote(self.path.split("?", 1)[0])
        if path == "/":
            self.send_bytes(INDEX_HTML.encode("utf-8"), "text/html; charset=utf-8")
            return
        if path == "/api/images":
            self.send_json(self.app.image_index())
            return
        if path.startswith("/api/labels/"):
            image_name = Path(path.removeprefix("/api/labels/")).name
            self.send_json(self.app.read_label(image_name))
            return
        if path.startswith("/api/delete/"):
            self.send_json({"error": "method not allowed"}, 405)
            return
        if path.startswith("/image/"):
            image_name = Path(path.removeprefix("/image/")).name
            image_path = self.app.images_dir / image_name
            if not image_path.exists():
                self.send_json({"error": "image not found"}, 404)
                return
            content_type = mimetypes.guess_type(image_path.name)[0] or "application/octet-stream"
            self.send_bytes(image_path.read_bytes(), content_type)
            return
        self.send_json({"error": "not found"}, 404)

    def do_POST(self):
        path = unquote(self.path.split("?", 1)[0])
        if not path.startswith("/api/labels/"):
            if path.startswith("/api/delete/"):
                image_name = Path(path.removeprefix("/api/delete/")).name
                try:
                    deleted = self.app.delete_image(image_name)
                except Exception as exc:
                    self.send_json({"error": str(exc)}, 400)
                    return
                self.send_json({"ok": True, "deleted": deleted})
                return
            self.send_json({"error": "not found"}, 404)
            return
        image_name = Path(path.removeprefix("/api/labels/")).name
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length)
        try:
            boxes = json.loads(raw.decode("utf-8"))
            self.app.write_label(image_name, boxes)
        except Exception as exc:
            self.send_json({"error": str(exc)}, 400)
            return
        self.send_json({"ok": True})

    def log_message(self, fmt, *args):
        print(f"{self.address_string()} - {fmt % args}")


class AnnotatorApp:
    def __init__(self, annotation_dir: Path):
        self.annotation_dir = annotation_dir
        self.images_dir = annotation_dir / "images"
        self.labels_dir = annotation_dir / "labels"
        self.deleted_dir = annotation_dir / "deleted"
        self.deleted_images_dir = self.deleted_dir / "images"
        self.deleted_labels_dir = self.deleted_dir / "labels"
        self.classes_path = annotation_dir / "classes.txt"
        self.manifest_path = annotation_dir / "annotation_manifest.csv"
        self.labels_dir.mkdir(parents=True, exist_ok=True)
        self.deleted_images_dir.mkdir(parents=True, exist_ok=True)
        self.deleted_labels_dir.mkdir(parents=True, exist_ok=True)
        self.classes = [line.strip() for line in self.classes_path.read_text(encoding="utf-8").splitlines() if line.strip()]
        self.negative_hints = self._load_negative_hints()

    def _load_negative_hints(self):
        hints = set()
        if not self.manifest_path.exists():
            return hints
        import csv
        with self.manifest_path.open(newline="", encoding="utf-8") as f:
            for row in csv.DictReader(f):
                if row.get("negative_hint") == "1":
                    hints.add(row.get("image_name", ""))
        return hints

    def label_path_for(self, image_name: str) -> Path:
        return self.labels_dir / f"{Path(image_name).stem}.txt"

    def image_index(self):
        images = []
        for image_path in sorted(self.images_dir.glob("*.jpg")):
            label_path = self.label_path_for(image_path.name)
            images.append(
                {
                    "name": image_path.name,
                    "labeled": label_path.exists(),
                    "negative_hint": image_path.name in self.negative_hints,
                }
            )
        return {"classes": self.classes, "images": images}

    def read_label(self, image_name: str):
        label_path = self.label_path_for(image_name)
        boxes = []
        if not label_path.exists():
            return boxes
        for line in label_path.read_text(encoding="utf-8").splitlines():
            if not line.strip():
                continue
            parts = line.split()
            if len(parts) != 5:
                continue
            cls, x, y, w, h = parts
            boxes.append({"cls": int(cls), "x": float(x), "y": float(y), "w": float(w), "h": float(h)})
        return boxes

    def write_label(self, image_name: str, boxes):
        image_path = self.images_dir / Path(image_name).name
        if not image_path.exists():
            raise FileNotFoundError(image_name)
        lines = []
        for box in boxes:
            cls = int(box["cls"])
            if cls < 0 or cls >= len(self.classes):
                raise ValueError(f"invalid class id: {cls}")
            values = [float(box[key]) for key in ("x", "y", "w", "h")]
            if any(value < 0 or value > 1 for value in values):
                raise ValueError(f"box out of range: {box}")
            if values[2] <= 0 or values[3] <= 0:
                raise ValueError(f"box has non-positive size: {box}")
            lines.append(f"{cls} {values[0]:.6f} {values[1]:.6f} {values[2]:.6f} {values[3]:.6f}")
        self.label_path_for(image_name).write_text("\n".join(lines) + ("\n" if lines else ""), encoding="utf-8")

    def delete_image(self, image_name: str):
        basename = Path(image_name).name
        image_path = self.images_dir / basename
        if not image_path.exists():
            raise FileNotFoundError(basename)

        deleted_image_path = self.deleted_images_dir / basename
        deleted_label_path = self.deleted_labels_dir / f"{Path(basename).stem}.txt"
        label_path = self.label_path_for(basename)

        shutil.move(str(image_path), str(deleted_image_path))
        if label_path.exists():
            shutil.move(str(label_path), str(deleted_label_path))
        return str(deleted_image_path)


class AnnotatorServer(ThreadingHTTPServer):
    def __init__(self, server_address, handler, app):
        super().__init__(server_address, handler)
        self.app = app


def main() -> int:
    parser = argparse.ArgumentParser(description="Run a tiny local YOLO annotation web app.")
    parser.add_argument("--annotation-dir", type=Path, default=DEFAULT_ANNOTATION_DIR)
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    args = parser.parse_args()

    app = AnnotatorApp(args.annotation_dir)
    server = AnnotatorServer((args.host, args.port), AnnotatorHandler, app)
    print(f"annotation_dir: {args.annotation_dir}", flush=True)
    print(f"images: {len(app.image_index()['images'])}", flush=True)
    print(f"classes: {', '.join(app.classes)}", flush=True)
    print(f"url: http://{args.host}:{args.port}/", flush=True)
    print("Press Ctrl+C to stop.", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping annotator.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
