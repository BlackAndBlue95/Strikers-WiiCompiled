# source scripts/msc/env.sh — puts the translator on PATH as `translator`.
# Needs the .NET 8 SDK (dotnet) and a Release build of translator/src/Translator.Cli:
#   dotnet build translator/src/Translator.Cli -c Release
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-${(%):-%x}}")/../.." && pwd)"
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1
translator() { dotnet "$REPO_ROOT/translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll" "$@"; }
