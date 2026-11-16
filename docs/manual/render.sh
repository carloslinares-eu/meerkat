#!/bin/sh
# Renders the manual's diagrams to SVG next to their sources.
# Needs PlantUML (Debian: apt install plantuml). Commit the SVGs too: the
# manual and the Doxygen pages show them without PlantUML.
set -eu
cd "$(dirname "$0")"
plantuml -tsvg -nometadata workflow.puml campaign-day.puml
