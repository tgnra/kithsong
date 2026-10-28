"""python -m ssg build | serve"""

import argparse
import sys

from .build import build
from .markdown_ext import BuildError


def main(argv=None):
    parser = argparse.ArgumentParser(prog="ssg")
    sub = parser.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build", help="build the site into dist/")
    b.add_argument("--drafts", action="store_true", help="include draft docs pages")
    b.add_argument("--base-url", help="override base_url from site.toml")
    s = sub.add_parser("serve", help="build, serve dist/ and rebuild on change")
    s.add_argument("--port", type=int, default=8000)
    s.add_argument("--drafts", action="store_true")
    s.add_argument("--base-url", help="override base_url from site.toml")
    args = parser.parse_args(argv)

    if args.cmd == "build":
        try:
            build(".", drafts=args.drafts, base_url=args.base_url)
        except (BuildError, ValueError) as e:
            print(f"error: {e}", file=sys.stderr)
            return 1
    elif args.cmd == "serve":
        from .serve import serve
        serve(".", port=args.port, drafts=args.drafts, base_url=args.base_url)
    return 0


if __name__ == "__main__":
    sys.exit(main())
