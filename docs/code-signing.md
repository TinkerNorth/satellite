# Windows code signing (Azure Artifact Signing)

The release workflow signs `satellite.exe` and `SatelliteSetup.exe` with
[Azure Artifact Signing](https://azure.microsoft.com/en-us/products/artifact-signing)
(formerly Trusted Signing). Signing is keyless: the workflow exchanges its
GitHub OIDC token for Entra credentials at run time and submits file digests
to the signing service. No certificate or private key is ever stored in this
repository or its secrets — the key lives in Microsoft's HSMs and the
short-lived certificates chain to a Microsoft-trusted root, which carries
SmartScreen reputation from day one (this is what stops the
`Trojan:Win32/Wacatac.B!ml` false positives on unsigned builds).

Until the secrets below exist, releases still work: the signing steps skip
and artifacts get the `-unsigned` suffix. Stable (non-prerelease) tags refuse
to release unsigned — see the `required-secrets` gate in `release.yml`.

Cost: Basic tier, $9.99/month, 5,000 signatures included.

## One-time onboarding

### 1. GitHub: create the `release` environment

Repo → Settings → Environments → New environment → name it exactly
`release`. No protection rules are required (add a required reviewer later
if you want manual approval before anything gets signed). The `windows` job
runs in this environment so the OIDC token's subject claim is stable across
tags (`repo:TinkerNorth/satellite:environment:release`) — federated
credentials are exact-match, and per-tag subjects would never match.

### 2. Azure: subscription and signing account

1. Create an Azure account + subscription if you don't have one
   (azure.com, free tier is fine; the signing service bills to the
   subscription).
2. Portal → search "Artifact Signing" (may still appear as "Trusted
   Signing") → Create. Pick a resource group, an account name (e.g.
   `tinkernorth-signing`), a region close to you, and the **Basic** SKU.
   The region fixes your endpoint URL, e.g.:
   - East US → `https://eus.codesigning.azure.net/`
   - West Europe → `https://weu.codesigning.azure.net/`
3. Give yourself the **Trusted Signing Identity Verifier** role on the
   account (Access control (IAM) → Add role assignment) — needed to do the
   next step; being subscription Owner is not enough.

### 3. Azure: identity validation (the human part)

Account → Identity validations → New → **Individual**. You'll submit a
government-issued photo ID plus a selfie/biometric check; the name and
address on the validation request must match the ID exactly. Self-employed
individuals in US/CA/EU/UK are eligible. Allow one to three business days.
This produces the legal identity that appears in your certificates'
Subject (`CN=<your name>`).

### 4. Azure: certificate profile

Account → Certificate profiles → New → type **Public Trust**, link the
identity validation from step 3, name it (e.g. `satellite-public`). This is
the `AZURE_SIGN_PROFILE` value.

### 5. Entra: app registration with GitHub federation

1. Microsoft Entra ID → App registrations → New registration → name it
   (e.g. `satellite-release-signer`), single tenant, no redirect URI.
   Record the **Application (client) ID** and **Directory (tenant) ID**.
2. The app → Certificates & secrets → Federated credentials → Add:
   - Scenario: **GitHub Actions deploying Azure resources**
   - Organization: `TinkerNorth`, Repository: `satellite`
   - Entity type: **Environment**, Environment name: `release`
   - The subject must read `repo:TinkerNorth/satellite:environment:release`
   Do NOT create a client secret — the federation replaces it.
3. Back on the signing account → Access control (IAM) → Add role
   assignment → role **Code Signing Certificate Profile Signer** → assign
   to the `satellite-release-signer` app.

### 6. GitHub: repository secrets

Repo → Settings → Secrets and variables → Actions → New repository secret,
six of them:

| Secret                  | Value                                              |
| ----------------------- | -------------------------------------------------- |
| `AZURE_TENANT_ID`       | Directory (tenant) ID from step 5                  |
| `AZURE_CLIENT_ID`       | Application (client) ID from step 5                |
| `AZURE_SUBSCRIPTION_ID` | Subscription ID hosting the signing account        |
| `AZURE_SIGN_ENDPOINT`   | Regional endpoint URL from step 2                  |
| `AZURE_SIGN_ACCOUNT`    | Signing account name from step 2                   |
| `AZURE_SIGN_PROFILE`    | Certificate profile name from step 4               |

None of the last three are sensitive, but keeping all six as secrets lets
the `required-secrets` gate check them uniformly.

### 7. Verify with a prerelease

Push a prerelease tag (or Actions → Release → Run workflow with an existing
tag). The windows job should show `Azure login`, both signing steps, and a
green `Verify Authenticode signatures` step, and the staged installer loses
its `-unsigned` suffix. Locally:

```powershell
Get-AuthenticodeSignature .\SatelliteSetup-vX.Y.Z.exe | Format-List
```

Status must be `Valid` with a Microsoft ID Verified CA chain.

## Notes

- The signed *installer* embeds a signed *satellite.exe* because the exe is
  signed before iscc packs it. Keep that step order.
- The uninstaller (`unins000.exe`) that Inno generates on end-user machines
  is still unsigned. Signing it requires wiring a `SignTool=` command into
  `installer.iss` at iscc time; tracked as a future refinement.
- Certificates are short-lived and rotate automatically; nothing expires on
  our side and there is nothing to renew except the identity validation
  (Azure emails before it lapses).
- macOS signing/notarization (`DEVELOPER_ID_*`, `APPLE_*` secrets) is a
  separate Apple Developer Program flow ($99/yr) and is still required by
  the gate for stable tags.
