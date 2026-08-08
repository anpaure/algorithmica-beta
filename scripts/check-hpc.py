#!/usr/bin/env python3
"""Validate an Algorithmica β build before it is published.

Usage:

    hugo --config config.beta.yaml --minify --destination /tmp/algorithmica-beta
    python3 scripts/check-hpc.py /tmp/algorithmica-beta
"""

from __future__ import annotations

import argparse
import collections
import html.parser
import json
import pathlib
import posixpath
import re
import sys
import urllib.parse


BASE_URL = urllib.parse.urlsplit(
    "https://anpaure.github.io/algorithmica-beta/"
)
BASE_PATH = BASE_URL.path.rstrip("/")
EXCLUDED_SECTIONS = {"distributed", "parallel", "slides"}
VALID_TAG = re.compile(r"^[a-z][a-z0-9:-]*$")
FENCE = re.compile(r"^\s*(`{3,}|~{3,})")
BEGIN_ENV = re.compile(r"\\begin\{([^}]+)\}")
END_ENV = re.compile(r"\\end\{([^}]+)\}")


class Document(html.parser.HTMLParser):
    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.references: list[tuple[str, str]] = []
        self.ids: set[str] = set()
        self.duplicate_ids: set[str] = set()
        self.invalid_tags: set[str] = set()
        self.katex_errors = 0

    def inspect_tag(
        self, tag: str, attrs: list[tuple[str, str | None]]
    ) -> None:
        if not VALID_TAG.fullmatch(tag):
            self.invalid_tags.add(tag)
        attributes = dict(attrs)
        identifier = attributes.get("id")
        if identifier:
            if identifier in self.ids:
                self.duplicate_ids.add(identifier)
            self.ids.add(identifier)
        classes = (attributes.get("class") or "").split()
        if "katex-error" in classes:
            self.katex_errors += 1
        if tag == "a" and attributes.get("href"):
            self.references.append(("link", attributes["href"] or ""))
        if tag in {"img", "script"} and attributes.get("src"):
            self.references.append((tag, attributes["src"] or ""))
        if tag == "link" and attributes.get("href"):
            self.references.append((tag, attributes["href"] or ""))

    def handle_starttag(
        self, tag: str, attrs: list[tuple[str, str | None]]
    ) -> None:
        self.inspect_tag(tag, attrs)

    def handle_startendtag(
        self, tag: str, attrs: list[tuple[str, str | None]]
    ) -> None:
        self.inspect_tag(tag, attrs)


def source_files(root: pathlib.Path) -> list[pathlib.Path]:
    hpc = root / "content" / "english" / "hpc"
    return [
        path
        for path in sorted(hpc.rglob("*.md"))
        if not EXCLUDED_SECTIONS.intersection(path.relative_to(hpc).parts)
    ]


def source_route(root: pathlib.Path, source: pathlib.Path) -> str:
    relative = source.relative_to(root / "content" / "english")
    if source.name == "_index.md":
        parts = relative.parent.parts
    else:
        parts = relative.with_suffix("").parts
    return "/" + "/".join(parts) + "/"


def markdown_without_code(text: str) -> list[tuple[int, str]]:
    result: list[tuple[int, str]] = []
    fence: str | None = None
    for line_number, line in enumerate(text.splitlines(), 1):
        match = FENCE.match(line)
        if match:
            marker = match.group(1)[0]
            if fence is None:
                fence = marker
            elif fence == marker:
                fence = None
            continue
        if fence is not None:
            continue
        # Inline code is not input to KaTeX. This intentionally follows the
        # common one-backtick form used throughout the book.
        prose = "".join(line.split("`")[::2])
        result.append((line_number, prose))
    return result


def unescaped_dollars(line: str) -> list[int]:
    result: list[int] = []
    index = 0
    while index < len(line):
        if line[index] != "$" or (index and line[index - 1] == "\\"):
            index += 1
            continue
        if index + 1 < len(line) and line[index + 1] == "$":
            index += 2
            continue
        result.append(index)
        index += 1
    return result


def validate_math_source(
    root: pathlib.Path, source: pathlib.Path, errors: list[str]
) -> None:
    relative = source.relative_to(root)
    lines = markdown_without_code(source.read_text(encoding="utf-8"))
    prose = "\n".join(line for _, line in lines)
    if prose.count("$$") % 2:
        errors.append(f"unbalanced display-math delimiter: {relative}")

    begins = collections.Counter(BEGIN_ENV.findall(prose))
    ends = collections.Counter(END_ENV.findall(prose))
    if begins != ends:
        errors.append(
            f"unbalanced TeX environment: {relative} "
            f"(begin={dict(begins)}, end={dict(ends)})"
        )

    for line_number, line in lines:
        dollars = unescaped_dollars(line)
        if len(dollars) % 2 == 0:
            continue
        # The original text uses three unmatched currency markers: $25,
        # $200M, and $429,496.7295, plus a $/GB table heading. KaTeX does not
        # pair these with a delimiter on another line.
        tail = line[dollars[-1] + 1 :]
        if re.match(r"(?:\d|/)", tail):
            continue
        errors.append(f"unbalanced inline math: {relative}:{line_number}")


def parse_html(path: pathlib.Path) -> Document:
    parser = Document()
    parser.feed(path.read_text(encoding="utf-8"))
    return parser


def strip_base_path(path: str) -> str:
    if path == BASE_PATH:
        return "/"
    if path.startswith(BASE_PATH + "/"):
        return path[len(BASE_PATH) :]
    return path


def local_target(
    build: pathlib.Path, page: pathlib.Path, href: str
) -> tuple[pathlib.Path, str] | None:
    parsed = urllib.parse.urlsplit(href)
    if parsed.scheme in {"mailto", "data", "javascript", "tel"}:
        return None
    if parsed.scheme or parsed.netloc:
        if (
            parsed.scheme not in {"http", "https"}
            or parsed.netloc != BASE_URL.netloc
            or not parsed.path.startswith(BASE_PATH)
        ):
            return None

    path = urllib.parse.unquote(parsed.path)
    if path.startswith("/"):
        path = strip_base_path(path)
        target = build / path.lstrip("/")
    elif path:
        relative = page.relative_to(build).as_posix()
        joined = posixpath.normpath(
            posixpath.join(posixpath.dirname(relative), path)
        )
        target = build / joined
    else:
        target = page

    if path.endswith("/") or target.is_dir():
        target /= "index.html"
    elif not target.suffix and not target.exists():
        target /= "index.html"
    return target, urllib.parse.unquote(parsed.fragment)


def main() -> int:
    argument_parser = argparse.ArgumentParser()
    argument_parser.add_argument(
        "build", type=pathlib.Path, help="Hugo beta destination directory"
    )
    argument_parser.add_argument(
        "--root",
        type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parent.parent,
        help="repository root",
    )
    arguments = argument_parser.parse_args()
    root = arguments.root.resolve()
    build = arguments.build.resolve()
    errors: list[str] = []

    sources = source_files(root)
    expected_routes = {source_route(root, source) for source in sources}
    for source in sources:
        validate_math_source(root, source, errors)

    search_path = build / "searchindex.json"
    if not search_path.is_file():
        errors.append(f"missing search index: {search_path}")
        indexed_routes: set[str] = set()
    else:
        index = json.loads(search_path.read_text(encoding="utf-8"))
        indexed_routes = {
            strip_base_path(urllib.parse.urlsplit(item["path"]).path)
            for item in index
        }
        if len(indexed_routes) != len(index):
            errors.append("duplicate route in search index")
        outside = sorted(
            route for route in indexed_routes if not route.startswith("/hpc/")
        )
        if outside:
            errors.append(f"non-HPC routes in search index: {outside}")
        for route in sorted(expected_routes - indexed_routes):
            errors.append(f"source missing from search index: {route}")
        for route in sorted(indexed_routes - expected_routes):
            errors.append(f"unexpected route in search index: {route}")

    for section in EXCLUDED_SECTIONS:
        if (build / "hpc" / section).exists():
            errors.append(f"excluded section was rendered: /hpc/{section}/")

    html_files = sorted(build.rglob("*.html"))
    documents: dict[pathlib.Path, Document] = {}
    if not html_files:
        errors.append(f"no rendered HTML below: {build}")
    for page in html_files:
        text = page.read_text(encoding="utf-8")
        relative = page.relative_to(build)
        if "Algorithmica β" not in text:
            errors.append(f"missing beta brand: {relative}")
        document = documents.setdefault(page, parse_html(page))
        for tag in document.invalid_tags:
            errors.append(f"invalid HTML tag: {relative} -> <{tag}>")
        for identifier in document.duplicate_ids:
            errors.append(f"duplicate anchor: {relative} -> #{identifier}")
        if document.katex_errors:
            errors.append(
                f"rendered KaTeX error: {relative} ({document.katex_errors})"
            )

        for kind, href in document.references:
            resolved = local_target(build, page, href)
            if resolved is None:
                continue
            target, fragment = resolved
            if not target.is_file():
                errors.append(f"missing rendered {kind}: {relative} -> {href}")
                continue
            if fragment and target.suffix == ".html":
                target_document = documents.setdefault(target, parse_html(target))
                if fragment not in target_document.ids:
                    errors.append(f"missing anchor: {relative} -> {href}")

    if errors:
        print("Algorithmica β validation failed:", file=sys.stderr)
        for error in sorted(set(errors)):
            print(f"  {error}", file=sys.stderr)
        return 1

    print(
        "Algorithmica β validation passed: "
        f"{len(sources)} Part I sources, {len(indexed_routes)} indexed routes, "
        f"and {len(html_files)} HTML files"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
