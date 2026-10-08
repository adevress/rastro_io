# MeasurementSet v4 (MSv4) — specification notes

This document summarises the MeasurementSet v4 data model as implemented by
[XRADIO](https://github.com/casangi/xradio) and as stored on disk in the Zarr v3
format. It is the reference used by the C++ MSv4 reader in `src/msv4/`.

Primary sources:

* XRADIO overview and data-model documentation
  <https://xradio.readthedocs.io/en/stable/measurement_set/overview.html>
  and the schema page `measurement_set/schema.html`.
* XRADIO `convert_msv2_to_processing_set` implementation and the Zarr stores it
  produces (`zarr.json` metadata, chunk layout).
* MeasurementSet definition version 2.0 (casacore), from which MSv4 derives.
* MSv4 Review Panel report (late 2024 / early 2025).

## 1. Design goals and relation to MSv2

MSv2 is a set of casacore tables (MAIN plus subtables) whose rows carry
variable-shaped cells. MSv4 replaces the row/tables model with **labelled
n-dimensional arrays**:

| MSv2 table | MSv4 access | Array type |
| --- | --- | --- |
| MAIN | `ms_xdt` / correlated dataset | DataTree node |
| SCAN | `scan_name` | coordinate |
| POLARIZATION | `polarization` | coordinate |
| SPECTRAL_WINDOW | `frequency` | coordinate |
| DOPPLER | `frequency` | coordinate |
| FIELD, SOURCE, EPHEMERIDES | `field_and_source_*_xds` | sub-dataset |
| OBSERVATION | `observation_info` | dict (attribute) |
| PROCESSOR | `processor_info` | dict (attribute) |
| ANTENNA, FEED | `antenna_xds` | sub-dataset |
| POINTING | `pointing_xds` | sub-dataset |
| SYSCAL | `system_calibration_xds` | sub-dataset |
| WEATHER | `weather_xds` | sub-dataset |
| GAIN_CURVE | `gain_curve_xds` | sub-dataset |
| PHASE_CAL | `phase_calibration_xds` | sub-dataset |
| PHASED_ARRAY | `phased_array_xds` | sub-dataset |

Key consequences:

* A single MSv4 is **fully self-describing** and covers exactly one
  `(observation, spectral window, polarization setup, observation mode,
  processor, beam-per-antenna)` combination. The MSv2 *data description* (DDI)
  is therefore deprecated: finer information is encoded by partitioning.
* Rows disappear. The MSv2 `DATA` cell of shape
  `(ncorr, nchan)` becomes a `VISIBILITY(time, baseline_id, frequency,
  polarization)` array. `time × baseline` replaces the row axis.
* Implicit numbered indices are replaced by descriptive, labelled coordinates
  (`antenna_name` instead of `antenna_id`, `baseline_id`, `frequency`, ...).
* Missing baselines / dropped integrations are represented by **NaN padding**
  so that array shapes remain regular (`frequency` is the exception: channels
  are dense).
* `WEIGHT` is redefined to be spectral (`WEIGHT_SPECTRUM`), always with the same
  shape as `VISIBILITY`. `SIGMA`, `FLAG_ROW`, `FLAG_CATEGORY` and
  `BASELINE_REFERENCE` are dropped (flag versioning is handled by data groups).

## 2. Processing Set and MSv4 DataTree

A **Processing Set (PS)** groups several MSv4 datasets that belong to the same
observation campaign. On disk it is a Zarr v3 group whose direct children are
the MSv4 nodes:

```
<ps>.ps.zarr/                      (root group, attrs.type = "processing_set")
└── visibility.scan-400_0/         (MSv4 group, attrs.type = "visibility")
    ├── time/                      (1-D coordinate array)
    ├── frequency/                 (1-D coordinate array)
    ├── polarization/              (1-D coordinate array, fixed_length_utf32)
    ├── baseline_id/               (1-D coordinate array)
    ├── VISIBILITY/                (4-D data variable, complex64)
    ├── FLAG/                      (4-D data variable, bool)
    ├── WEIGHT/                    (4-D data variable, float32)
    ├── UVW/                       (3-D data variable, float64)
    ├── EFFECTIVE_INTEGRATION_TIME/
    ├── TIME_CENTROID/
    ├── antenna_xds/               (sub-groups / sub-datasets)
    ├── field_and_source_base_xds/
    └── phased_array_xds/
```

Each node stores a `zarr.json`. The MSv4 group `attributes` carry the
**info dictionaries** and the `data_groups` mapping (see §5).

## 3. The correlated dataset (main MSv4)

### 3.1 Dimensions

| dimension | meaning |
| --- | --- |
| `time` | integration (row centroid) index |
| `baseline_id` | baseline index (ordered antenna pair) |
| `frequency` | spectral channel |
| `polarization` | correlation product |
| `uvw_label` | 3 (`u`, `v`, `w`) |
| `sky_dir_label` | 2 (`ra`, `dec`) |

`baseline_id` is dense and enumerates all antenna pairs present in the
observation. `baseline_antenna1_name` and `baseline_antenna2_name` are
coordinate arrays giving the two `antenna_name`s of each baseline.

### 3.2 Data variables

Only `VISIBILITY` (interferometry) or `SPECTRUM` (single dish) is mandatory;
the others may be absent and are optionally versioned through data groups.

| name | dims | dtype | meaning |
| --- | --- | --- | --- |
| `VISIBILITY` | time, baseline_id, frequency, polarization | complex | complex visibility `DATA` |
| `SPECTRUM` | time, baseline_id, frequency, polarization | float | single-dish spectrum |
| `FLAG` | time, baseline_id, frequency, polarization | bool | per-visibility flags |
| `WEIGHT` | time, baseline_id, frequency, polarization | float | `1/sigma^2`, per channel |
| `UVW` | time, baseline_id, uvw_label | float | `(u,v,w)` in metres, frame attribute |
| `TIME_CENTROID` | time, baseline_id | float | centroid time when it differs from `time` |
| `EFFECTIVE_INTEGRATION_TIME` | time, baseline_id | float | effective integration time |

### 3.3 Coordinates

| name | dims | meaning |
| --- | --- | --- |
| `time` | time | integration midpoint; `attrs.units="s"`, `scale="utc"`, `format="unix"` |
| `frequency` | frequency | channel centre in Hz; `attrs.reference_frequency`, `attrs.channel_width` |
| `polarization` | polarization | Stokes/correlation labels (`XX`, `XY`, ...) |
| `baseline_id` | baseline_id | baseline index |
| `baseline_antenna1_name`, `baseline_antenna2_name` | baseline_id | antenna names |
| `field_name` | time | field name per integration |
| `scan_name` | time | scan name per integration |
| `uvw_label` | uvw_label | `u`,`v`,`w` |

## 4. Sub-datasets

### 4.1 `antenna_xds`

| variable / coord | dims | meaning |
| --- | --- | --- |
| `antenna_name` | antenna_name | unique antenna name |
| `station_name` | antenna_name | station name |
| `mount` | antenna_name | mount type |
| `telescope_name` | antenna_name | telescope name |
| `ANTENNA_POSITION` | antenna_name, cartesian_pos_label | geocentric `(x,y,z)` in m, frame `ITRS` |
| `ANTENNA_DISH_DIAMETER` | antenna_name | dish diameter in m |
| `ANTENNA_RECEPTOR_ANGLE` | antenna_name, receptor_label | receptor angles in rad |
| `polarization_type` | antenna_name, receptor_label | feed polarization (e.g. `X`, `Y`) |

Group attributes carry `type="antenna"` and `overall_telescope_name`.

### 4.2 `field_and_source_*_xds`

| variable / coord | dims | meaning |
| --- | --- | --- |
| `field_name` | field_name | field name |
| `source_name` | field_name | source name (`Unknown` when the MSv2 SOURCE table is empty) |
| `FIELD_PHASE_CENTER_DIRECTION` | field_name, sky_dir_label | phase centre in rad, frame attribute |
| `FIELD_REFERENCE_CENTER_DIRECTION` | field_name, sky_dir_label | reference centre (single dish) |

Ephemeris observations extend this dataset with time-dependent
`*_EPHEMERIS` variables; `attrs.type` becomes `field_and_source_ephemeris`.

### 4.3 Optional sub-datasets

`pointing_xds`, `system_calibration_xds`, `gain_curve_xds`,
`phase_calibration_xds`, `weather_xds` and `phased_array_xds` follow the same
pattern: labelled dimensions, capitalised data variables, documented `attrs`.

## 5. Data groups and info dictionaries

`data_groups` is an attribute of the MSv4 node mapping a group name to the
names of the variables that make up a consistent set:

```json
{
  "base": {
    "correlated_data": "VISIBILITY",
    "flag": "FLAG",
    "weight": "WEIGHT",
    "uvw": "UVW",
    "field_and_source": "field_and_source_base_xds"
  }
}
```

New versions of a variable use the standard name plus a suffix
(`VISIBILITY_CORRECTED`, `WEIGHT_IMAGING`, ...). The reader selects a group by
name (default: `base`, else the first available).

The MSv4 node attributes also contain `observation_info` and `processor_info`
dictionaries (from OBSERVATION / PROCESSOR), for example:

```json
"observation_info": { "observer": ["vm462"], "project_UID": "", ... }
"processor_info":   { "type": "", "sub_type": "" }
```

## 6. On-disk Zarr v3 representation

XRADIO writes the processing set with `zarr_format = 3`. Every array and group
has its own `zarr.json`. An array's metadata has the shape

```json
{
  "shape": [900, 2278, 416, 4],
  "data_type": "complex64",
  "chunk_grid": { "name": "regular",
                  "configuration": { "chunk_shape": [32, 2278, 416, 4] } },
  "chunk_key_encoding": { "name": "default",
                          "configuration": { "separator": "/" } },
  "fill_value": [0.0, 0.0],
  "codecs": [
    { "name": "bytes", "configuration": { "endian": "little" } },
    { "name": "blosc", "configuration": { "typesize": 8, "cname": "lz4",
                                          "clevel": 5, "shuffle": "noshuffle",
                                          "blocksize": 0 } }
  ],
  "attributes": { "type": "quanta", "units": "unknown" },
  "dimension_names": ["time", "baseline_id", "frequency", "polarization"],
  "zarr_format": 3,
  "node_type": "array",
  "storage_transformers": []
}
```

Points relevant to a reader:

* **`data_type`** is either a string (`"bool"`, `"int64"`, `"float32"`,
  `"float64"`, `"complex64"`, ...) or an object for extension types, e.g.
  `{"name": "fixed_length_utf32", "configuration": {"length_bytes": 96}}`.
* **`codecs`** is an ordered pipeline. XRADIO uses
  `bytes` (endianness) followed by either `blosc` (bulk visibility data) or
  `zstd` (small coordinate arrays), or nothing (raw).
* **Chunk keys** with the default encoding are `<array>/c/<i>/<j>/...`, one
  directory level per dimension. A missing chunk is the fill value.
* **`fill_value`** may be `NaN`, a scalar, a JSON array (complex/two-valued),
  or a string.
* Groups carry `"node_type": "group"` plus `attributes`; the root additionally
  carries an inline **consolidated metadata** block with the metadata of every
  descendant, which allows a reader to enumerate the hierarchy without walking
  the filesystem. Each descendant `zarr.json` is nevertheless authoritative.
* String coordinates are stored as `fixed_length_utf32` (little-endian UTF-32,
  zero padded to `length_bytes`).

### 6.1 Units and conventions

* `time`: Unix seconds (UTC); MJD = `40587 + time/86400`.
* `frequency`: Hz; the channel width is `attrs.channel_width` and the reference
  frequency is `attrs.reference_frequency` (both quantities with units).
* `UVW`: metres, `frame` attribute (typically `fk5`).
* `ANTENNA_POSITION`: metres, `frame="ITRS"`, `coordinate_system="geocentric"`.
* Directions: radians, `frame` attribute.

## 7. Versioning

The schema version is stored as `attrs.schema_version` on every MSv4 node
(currently `4.0.0`). Each independent part of the schema is versioned on its
own; a reader must accept a node when it understands the parts it needs and
ignore unknown optional attributes/datasets.

## 8. Reader requirements derived from this spec

The C++ reader in `src/msv4/` therefore provides:

1. Opening a processing-set directory and enumerating its MSv4 partitions
   (from the consolidated metadata when available, otherwise the filesystem).
2. Lazy, metadata-only access to every array: shape, chunk shape, dtype,
   codecs, dimension names and attributes — no bulk data is read.
3. A chunk-aware, multi-dimensional slice API
   (`read(slice<time>, slice<baseline>, slice<frequency>, slice<polarization>)`)
   that touches only the chunks intersecting the requested region, so that a
   sub-region of a multi-hundred-GB measurement can be read with a bounded
   memory footprint.
4. Special accessors for the small coordinate and sub-dataset arrays needed to
   build an MSv4 summary equivalent to the MSv2 one.
