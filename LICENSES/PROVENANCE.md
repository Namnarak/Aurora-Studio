# Source and license provenance

Aurora Studio is a GPL-3.0-only product. `LICENSE` applies to the Studio
launcher, provisioning and Studio runtime, Studio MCP, and Studio-specific
packaging and documentation authored for this product. The pre-split local
source checkout records NamKrub-authored Studio work under full commit IDs
`3b18378ecf3a8549a137648fb1672142ca8b4010`,
`91b7a5ba57bed27be574852a0e21e378b1446018`, and
`fbb76ef3a41c4748fe4fad30da64375fb9cba3e5`. These are local provenance
references, not public GitHub commits; the publication snapshot intentionally
has no Git history.

Vinegar is the reference Linux Studio launcher at
[`vinegarhq/vinegar`](https://github.com/vinegarhq/vinegar), which uses
GPL-3.0. The pre-split source checkout did not contain a Vinegar submodule,
vendored source tree, or exact Vinegar commit that can be verified. This
repository therefore does not claim a specific Vinegar source revision.

Files retaining Apache-2.0 are enumerated individually in
[`APACHE-FILES.txt`](APACHE-FILES.txt). Each row records its source path and
full source commit (or a full local authoring hash), the license, and the
SHA-256 of the published file. The inventory includes files copied from the
Mocktail Apache-2.0 tree at
[`2451264696d83bdb91d1517a7107273213178aca`](https://github.com/komaruworld/mocktail/commit/2451264696d83bdb91d1517a7107273213178aca),
renamed Mocktail paths whose source revision is recorded row by row, and
NamKrub-authored shared modules explicitly assigned Apache-2.0 for Aurora
Player and retained with that license here. `LICENSE-APACHE` contains the
Apache-2.0 terms for those files.

Studio's package installs the full GPL and Apache license texts, the Apache
file inventory, this provenance record, and third-party notices under
`share/doc/aurora-studio/`. Third-party licenses remain unchanged.
