#!/usr/bin/env bash
# Generates app/ios/ and app/android/ (the native RN project files that are
# not committed by default in this scaffold — see app/NATIVE_SETUP.md).
#
# Usage: tools/bootstrap-app-native.sh
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
app_dir="$repo_root/app"
rn_version="$(node -p "require('$app_dir/package.json').dependencies['react-native']")"

if [[ -d "$app_dir/ios" || -d "$app_dir/android" ]]; then
  echo "app/ios or app/android already exists — remove it first if you want to regenerate." >&2
  exit 1
fi

scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

# Names must match what the checked-in native projects already use, or a
# regeneration silently produces a project that no longer matches the Xcode
# scheme, the Gradle applicationId, or the bundle identifier registered with
# Apple. MotoCtrl is also app.json's `name` and getMainComponentName().
echo "Generating a bare RN ${rn_version} project in a scratch dir..."
npx --yes @react-native-community/cli init MotoCtrl \
  --version "${rn_version}" \
  --directory "$scratch/MotoCtrl" \
  --package-name com.motoctrl.app \
  --pm npm \
  --skip-git-init \
  --skip-install

cp -R "$scratch/MotoCtrl/ios" "$app_dir/ios"
cp -R "$scratch/MotoCtrl/android" "$app_dir/android"

echo "Done. Review app/ios and app/android, then:"
echo "  cd app && npm install && npx pod-install ios"
