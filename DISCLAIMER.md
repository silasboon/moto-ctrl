# Disclaimer

MOTO-CTRL is an open-source hobbyist project. Read this before you build, flash,
wire, or ride with it.

## Not a certified automotive device

MOTO-CTRL has **not** been certified, homologated, or approved by any regulatory
body (no ISO 26262 functional-safety certification, no DOT/ECE/UNECE type
approval, no CE/FCC/UKCA compliance testing beyond whatever informal checks the
maintainer chooses to publish). It is not a product built to automotive
functional-safety standards. Do not treat it as one.

## You assume all risk

By designing, building, flashing, wiring, installing, configuring, or riding a
motorcycle equipped with MOTO-CTRL, you accept full responsibility for:

- Verifying the wiring, fusing, and installation are correct for your specific
  motorcycle before riding it.
- Confirming your installation complies with the vehicle regulations in your
  jurisdiction (lighting, signaling, and electrical codes vary by country and
  state/province — this project does not track them for you).
- Any damage to your motorcycle, its electrical system, or other property arising
  from use, misuse, or malfunction of this hardware or software.
- Any injury to yourself or others arising from use, misuse, or malfunction of
  this hardware or software, including but not limited to failure of lighting,
  signaling, starting, or immobilizer functions.
- Testing thoroughly on a bench and in a controlled, stationary setting before
  ever riding with a new configuration or firmware version.

## No warranty

THE SOFTWARE, HARDWARE DESIGNS, AND DOCUMENTATION ARE PROVIDED "AS IS", WITHOUT
WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS, CONTRIBUTORS, OR THE MOTO-CTRL
PROJECT BE LIABLE FOR ANY CLAIM, DAMAGES, OR OTHER LIABILITY ARISING FROM,
OUT OF, OR IN CONNECTION WITH THE SOFTWARE, HARDWARE, OR THE USE OR OTHER
DEALINGS IN THEM — INCLUDING DAMAGE TO YOUR VEHICLE, PERSONAL INJURY, OR DEATH.
See the "No Liability" section of [`LICENSE-CODE`](LICENSE-CODE) and the
liability/warranty disclaimers in [`LICENSE-HARDWARE`](LICENSE-HARDWARE) for the
legally binding terms.

## Safety-critical wiring

MOTO-CTRL switches your motorcycle's lighting, signaling, ignition, and starter
circuits. A wiring mistake, a misconfigured output, or a firmware bug in a
modified fork can cause loss of lights, loss of signaling, an immobilized bike,
or unintended starter engagement. Always:

- Keep the stock ignition kill switch and any stock immobilizer functional and
  accessible as a fallback unless you fully understand what you are removing.
- Fuse every output appropriately for the connected load.
- Bench-test a new build (power, outputs, inputs) before installing it on the bike.
- Re-verify safety-critical behavior (lights, brake light, starter interlock)
  after every firmware update, on the bench, before riding.

## Project scope

This disclaimer applies to the MOTO-CTRL software (`firmware/`, `app/`), hardware
designs (`hardware/`, `enclosure/`), and documentation (`docs/`) in this
repository, and to any board — self-built or purchased assembled from the
MOTO-CTRL project — running this software or a derivative of it.
