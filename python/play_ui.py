"""Serve the local SVWB play board and bridge requests to the C++ JSON Lines CLI."""

import argparse
import json
import os
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import subprocess
import threading
from urllib.parse import urlsplit


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_WEIGHTS = ROOT / "data/phase4b-mirror/generation_025/weights.bin"
ASSETS = {
    "/": ("index.html", "text/html; charset=utf-8"),
    "/styles.css": ("styles.css", "text/css; charset=utf-8"),
    "/app.js": ("app.js", "text/javascript; charset=utf-8"),
}
MAX_REQUEST = 4096


def find_executable():
    name = "svwb_play_cli.exe" if os.name == "nt" else "svwb_play_cli"
    candidates = (
        ROOT / "build/phase5c-p0-release/Release" / name,
        ROOT / "build/Release" / name,
        ROOT / "build/phase5a-release/Release" / name,
        ROOT / "build/windows-release/Release" / name,
        ROOT / "build/windows-release" / name,
        ROOT / "build" / name,
    )
    return next((path for path in candidates if path.is_file()), candidates[0 if os.name == "nt" else -1])


class Engine:
    def __init__(self, command):
        self.lock = threading.Lock()
        self.process = subprocess.Popen(
            command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, encoding="utf-8",
            text=True, bufsize=1,
        )

    def request(self, command):
        with self.lock:
            try:
                self.process.stdin.write(json.dumps(command, ensure_ascii=True, allow_nan=False) + "\n")
                self.process.stdin.flush()
                line = self.process.stdout.readline()
                if not line:
                    raise RuntimeError("C++ 対局エンジンが終了しました。")
                return json.loads(line)
            except (OSError, UnicodeError, json.JSONDecodeError) as exc:
                raise RuntimeError("C++ 対局エンジンと通信できません。") from exc

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
        self.process.wait()
        self.process.stdin.close()
        self.process.stdout.close()


class EngineManager:
    def __init__(self, executable, simulations, seed, models, decks):
        self.lock = threading.Lock()
        self.command = [str(executable), "--simulations", str(simulations)]
        if seed is not None:
            self.command.extend(("--seed", str(seed)))
        self.models = models
        self.decks = decks
        self.model = "default"
        self.play_decks = ("default", "default")
        self.revision_offset = 0
        self.engine = self.start(self.model)

    def start(self, model):
        return Engine([*self.command, "--weights", str(self.models[model])])

    @staticmethod
    def selection(command, field, options):
        key = command.pop(field, "default")
        if not isinstance(key, str) or key not in options:
            raise ValueError("登録済みのデッキとモデルを選択してください。")
        return key

    def request(self, command):
        with self.lock:
            if command.get("command") == "new_game":
                model = self.selection(command, "model_id", self.models)
                chosen_decks = tuple(self.selection(command, field, self.decks) for field in ("deck_a", "deck_b"))
                for field, deck_id in zip(("deck_a", "deck_b"), chosen_decks):
                    deck = self.decks[deck_id]
                    if deck is not None:
                        command[field] = str(deck)
                candidate = self.engine if model == self.model else self.start(model)
                try:
                    result = candidate.request(command)
                except Exception:
                    if candidate is not self.engine:
                        candidate.close()
                    raise
                if candidate is not self.engine:
                    if result["ok"]:
                        previous, self.engine = self.engine, candidate
                        self.model = model
                        self.revision_offset += 2**32
                        previous.close()
                    else:
                        candidate.close()
                if result["ok"]:
                    self.play_decks = chosen_decks
            else:
                revision = command.get("revision")
                if type(revision) is int:
                    command["revision"] = max(0, revision - self.revision_offset)
                result = self.engine.request(command)
            state = result.get("state")
            if state is not None:
                state["model_id"] = self.model
                state["deck_a"], state["deck_b"] = self.play_decks
                state["revision"] += self.revision_offset
            for analysis in (result.get("analysis"), result.get("ai_turn"), (state or {}).get("last_ai_turn")):
                if analysis and "revision" in analysis:
                    analysis["revision"] += self.revision_offset
            return result

    def close(self):
        with self.lock:
            self.engine.close()


def list_choices(directory, pattern, default, label):
    paths = {"default": default}
    options = [{"id": "default", "label": label}]
    for path in sorted(directory.glob(pattern)):
        if not path.is_file() or path.resolve() == default:
            continue
        key = str(len(options))
        if path.suffix == ".json":
            try:
                deck = json.loads(path.read_text(encoding="utf-8-sig"))
                if not isinstance(deck.get("cards"), list):
                    continue
                name = deck.get("name") or path.stem
            except (OSError, UnicodeError, ValueError, AttributeError):
                continue
        else:
            name = path.relative_to(directory).parent.as_posix()
        paths[key] = path.resolve()
        options.append({"id": key, "label": name})
    return paths, options


class PlayServer(ThreadingHTTPServer):
    def __init__(self, port, engine, config):
        self.engine = engine
        self.config = config
        super().__init__(("127.0.0.1", port), PlayHandler)
        port = self.server_port
        self.hosts = {f"127.0.0.1:{port}", f"localhost:{port}"}
        if port == 80:
            self.hosts.update(("127.0.0.1", "localhost"))


class PlayHandler(BaseHTTPRequestHandler):
    def setup(self):
        super().setup()
        self.connection.settimeout(10)

    def respond(self, status, content_type, content):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(content)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Content-Security-Policy",
                         "default-src 'self'; object-src 'none'; frame-ancestors 'none'; base-uri 'none'")
        self.end_headers()
        self.wfile.write(content)

    def reply(self, status, result):
        data = json.dumps(result, ensure_ascii=False).encode("utf-8")
        self.respond(status, "application/json; charset=utf-8", data)

    def error(self, status, message):
        self.reply(status, {"ok": False, "error": {"code": status.name.lower(), "message": message}})

    def local_request(self):
        host = self.headers.get("Host", "").lower()
        origin = self.headers.get("Origin")
        if host not in self.server.hosts or origin not in (None, f"http://{host}"):
            self.error(HTTPStatus.FORBIDDEN, "ローカルの対戦画面から接続してください。")
            return False
        return True

    def do_GET(self):
        if not self.local_request():
            return
        path = urlsplit(self.path).path
        if path == "/api/config":
            self.reply(HTTPStatus.OK, self.server.config)
            return
        asset = ASSETS.get(path)
        if asset is None:
            self.error(HTTPStatus.NOT_FOUND, "ページが見つかりません。")
            return
        name, content_type = asset
        self.respond(HTTPStatus.OK, content_type, (ROOT / "web" / name).read_bytes())

    def do_POST(self):
        if not self.local_request():
            return
        if self.path != "/api/command":
            self.error(HTTPStatus.NOT_FOUND, "操作先が見つかりません。")
            return
        if self.headers.get_content_type() != "application/json":
            self.error(HTTPStatus.UNSUPPORTED_MEDIA_TYPE, "JSONを送信してください。")
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = 0
        if not 0 < length <= MAX_REQUEST:
            self.error(HTTPStatus.BAD_REQUEST, "リクエストサイズが正しくありません。")
            return
        try:
            payload = json.loads(self.rfile.read(length).decode("utf-8"))
            if not isinstance(payload, dict):
                raise ValueError
            result = self.server.engine.request(payload)
        except (ValueError, RecursionError) as exc:
            self.error(HTTPStatus.BAD_REQUEST, str(exc) or "JSONコマンドの形式が正しくありません。")
            return
        except RuntimeError as exc:
            self.error(HTTPStatus.BAD_GATEWAY, str(exc))
            return
        status = HTTPStatus.OK if result["ok"] else HTTPStatus.UNPROCESSABLE_ENTITY
        self.reply(status, result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--weights", type=Path, default=DEFAULT_WEIGHTS)
    parser.add_argument("--exe", type=Path, default=find_executable())
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--simulations", type=int, default=64)
    parser.add_argument("--seed", type=int)
    parser.add_argument("--deck-dir", type=Path, default=ROOT / "data/decks")
    parser.add_argument("--models-dir", type=Path, default=ROOT / "data")
    args = parser.parse_args()
    if not args.exe.is_file():
        parser.error(f"C++ play CLI not found: {args.exe}. Build svwb_play_cli in Release mode or specify --exe.")
    if not args.weights.is_file():
        parser.error(f"Weights not found: {args.weights}")
    if not 0 <= args.port <= 65535:
        parser.error("--port must be in 0..65535")
    if not 1 <= args.simulations <= 256:
        parser.error("--simulations must be in 1..256")
    if args.seed is not None and not 0 <= args.seed <= 2**53 - 1:
        parser.error("--seed must be in 0..9007199254740991")
    model_name = "第25世代モデル" if args.weights.resolve() == DEFAULT_WEIGHTS else args.weights.name
    models, model_options = list_choices(args.models_dir, "**/weights.bin", args.weights.resolve(), model_name)
    decks, deck_options = list_choices(args.deck_dir, "**/*.json", None, "確定ビショップ")
    engine = EngineManager(args.exe.resolve(), args.simulations, args.seed, models, decks)
    config = {"model": model_name, "simulations": args.simulations, "decks": deck_options, "models": model_options}
    try:
        engine.request({"command": "state"})
        with PlayServer(args.port, engine, config) as server:
            print(f"Local URL: http://127.0.0.1:{server.server_port}/", flush=True)
            server.serve_forever()
    except KeyboardInterrupt:
        pass
    except (OSError, RuntimeError) as exc:
        parser.exit(1, f"{exc}\n")
    finally:
        engine.close()


if __name__ == "__main__":
    main()
