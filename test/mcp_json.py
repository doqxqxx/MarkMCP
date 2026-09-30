"""mcp_json.py - tiny helper for test scripts: print a python expression over a JSON file.
Usage: py mcp_json.py <file> <expression>  e.g.  py mcp_json.py body.json "d['result']['id']"
"""
import json
import sys

d = json.load(open(sys.argv[1], encoding="utf-8"))
print(eval(sys.argv[2]))
