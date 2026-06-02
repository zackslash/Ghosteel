#!/usr/bin/env python3
"""Generate Qt .ts translation files from JSON translation data.

Usage:
    python3 generate_ts.py

Reads translations_XX.json files from the current directory and produces
ghosteel_XX.ts files. Each JSON file should have the structure:
{
    "ContextName": {
        "source string": "translated string",
        ...
    },
    ...
}
"""

import json
import os
import sys
import xml.etree.ElementTree as ET
from xml.dom import minidom


def generate_ts(lang_code, translations, output_path):
    """Generate a single .ts file for the given language."""
    ts = ET.Element("TS")
    ts.set("version", "2.1")
    ts.set("language", lang_code)

    for context_name, strings in sorted(translations.items()):
        context = ET.SubElement(ts, "context")
        name = ET.SubElement(context, "name")
        name.text = context_name

        for source, translation in sorted(strings.items()):
            # Detect plural forms (source contains %n)
            is_plural = "%n" in source

            message = ET.SubElement(context, "message")
            if is_plural:
                message.set("numerus", "yes")

            source_el = ET.SubElement(message, "source")
            source_el.text = source

            if is_plural:
                # Plural form: wrap in <numerusform> elements
                translation_el = ET.SubElement(message, "translation")
                numerus_form = ET.SubElement(translation_el, "numerusform")
                if translation:
                    numerus_form.text = translation
                else:
                    translation_el.set("type", "unfinished")
                    numerus_form.text = source  # fallback to source
            else:
                if translation:
                    translation_el = ET.SubElement(message, "translation")
                    translation_el.text = translation
                else:
                    translation_el = ET.SubElement(message, "translation")
                    translation_el.set("type", "unfinished")

    # Pretty-print with proper indentation
    rough = ET.tostring(ts, encoding="unicode", xml_declaration=False)
    xml_str = '<?xml version="1.0" encoding="utf-8"?>\n' + rough

    # Use minidom for pretty printing
    dom = minidom.parseString(xml_str)
    pretty = dom.toprettyxml(indent="    ", encoding=None)
    # Remove extra xml declaration added by toprettyxml
    lines = pretty.split("\n")
    if lines[0].startswith("<?xml"):
        lines[0] = '<?xml version="1.0" encoding="utf-8"?>'
    result = "\n".join(lines)

    with open(output_path, "w", encoding="utf-8") as f:
        f.write(result)
        f.write("\n")


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))

    # Find all translation JSON files
    json_files = sorted([
        f for f in os.listdir(script_dir)
        if f.startswith("translations_") and f.endswith(".json")
    ])

    if not json_files:
        print("No translations_*.json files found!", file=sys.stderr)
        sys.exit(1)

    print(f"Found {len(json_files)} translation files")

    for json_file in json_files:
        lang_code = json_file.replace("translations_", "").replace(".json", "")
        json_path = os.path.join(script_dir, json_file)
        ts_path = os.path.join(script_dir, f"ghosteel_{lang_code}.ts")

        with open(json_path, "r", encoding="utf-8") as f:
            translations = json.load(f)

        generate_ts(lang_code, translations, ts_path)
        total_strings = sum(len(v) for v in translations.values())
        print(f"  Generated ghosteel_{lang_code}.ts ({total_strings} strings, {len(translations)} contexts)")

    print("Done!")


if __name__ == "__main__":
    main()
