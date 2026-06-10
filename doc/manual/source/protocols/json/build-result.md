{{#include build-result-v1-fixed.md}}

## Examples

### Successful build

```json
{{#include schema/build-result-v1/success.json}}
```

### Failed build (output rejected)

```json
{{#include schema/build-result-v1/output-rejected.json}}
```

### Failed build (non-deterministic)

```json
{{#include schema/build-result-v1/not-deterministic.json}}
```

### Failed build (builder killed for memory)

```json
{{#include schema/build-result-v1/resource-exhausted.json}}
```

### Successful build (attached to an in-flight build)

```json
{{#include schema/build-result-v1/deduplicated.json}}
```