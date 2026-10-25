#!/usr/bin/env python3
"""Validate exact GPU admission, backend completion, and source return triples."""
import csv
import json
import sys

def validate(path):
    requests = {}
    with open(path, newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        expected = ["event", "id", "source", "tick_ps", "address", "bytes", "write"]
        if reader.fieldnames != expected:
            raise ValueError("unexpected live GPU trace schema")
        for row in reader:
            event = row["event"]
            if event not in ("INGRESS", "COMPLETED", "RETURNED"):
                raise ValueError(f"non-success GPU event: {event}")
            key = int(row["id"])
            fields = tuple(int(row[name]) for name in ("source", "address", "bytes", "write"))
            tick = int(row["tick_ps"])
            entry = requests.setdefault(key, {})
            if event in entry:
                raise ValueError(f"duplicate {event} id={key}")
            previous = {"COMPLETED": "INGRESS", "RETURNED": "COMPLETED"}.get(event)
            if previous:
                if previous not in entry or entry[previous][0] != fields:
                    raise ValueError(f"missing predecessor or changed source/payload id={key}")
                if tick < entry[previous][1]:
                    raise ValueError(f"completion/return before predecessor id={key}")
            entry[event] = (fields, tick)
    if not requests:
        raise ValueError("empty GPU trace is not a wiring validation")
    for key, entry in requests.items():
        if set(entry) != {"INGRESS", "COMPLETED", "RETURNED"}:
            raise ValueError(f"unfinished GPU request id={key}")
    return {"requests": len(requests), "completed": len(requests), "returned": len(requests),
            "sector32": sum(entry["INGRESS"][0][2] == 32 for entry in requests.values()),
            "sources": sorted({entry["INGRESS"][0][0] for entry in requests.values()}), "valid": True}

if __name__ == "__main__":
    print(json.dumps(validate(sys.argv[1]), sort_keys=True))
