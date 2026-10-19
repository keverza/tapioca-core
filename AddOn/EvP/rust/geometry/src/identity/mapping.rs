//! Pure structural validation for schema-v1 provenance tables.
//!
//! This gate checks declared identity and attribution relationships only. It
//! does not prove geometric contributor truth, perform semantic patch matching,
//! verify monotonic ID-allocation history, or repair immutable inputs. Those
//! responsibilities remain with the session owner.

use std::collections::{BTreeMap, BTreeSet, VecDeque};
use std::fmt;

use super::catalog::{
    BindingRecord, DOMAIN_EDGE, DOMAIN_FACE, DOMAIN_PATCH, DOMAIN_VERTEX, EntityCounts, KIND_EXACT,
    KIND_GENERATED, KIND_INHERITED, KIND_MERGED, KIND_SPLIT, LOGICAL_OBJECT, LOGICAL_OPERATION,
    LOGICAL_SOURCE, MappingRecord, Range, SupportTable, TableView, ValidationRequest,
};

const SCHEMA_VERSION: u32 = 1;
const DOMAIN_MASK_ALL: u32 = 0b1111;
const DOMAINS: [u32; 4] = [DOMAIN_VERTEX, DOMAIN_EDGE, DOMAIN_FACE, DOMAIN_PATCH];
const DASH_POSITIONS: [usize; 4] = [8, 13, 18, 23];

/// Stable provenance validation failure categories.
///
/// Use [`Self::message`] for a static diagnostic code prefix and description.
/// No variant reports partial success.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Error {
    /// Required domain coverage or contributor attribution is absent.
    ProvenanceUnattributable,
    /// An identifier, reference, range, discriminator, cycle or record conflicts.
    ProvenanceConflict,
    /// A current revision differs, or a historical/binding revision is in the future.
    ProvenanceStale,
    /// Two patch rows assign the same patch token to one object.
    PatchKeyUnstable,
}

impl Error {
    /// Return the stable diagnostic prefix and fixed description.
    #[must_use]
    pub const fn message(self) -> &'static str {
        match self {
            Self::ProvenanceUnattributable => {
                "ProvenanceUnattributable: required domain coverage or contributor attribution is missing"
            }
            Self::ProvenanceConflict => {
                "ProvenanceConflict: identity, reference, range, discriminator or cycle conflicts"
            }
            Self::ProvenanceStale => {
                "ProvenanceStale: current revision differs or historical/binding revision is in the future"
            }
            Self::PatchKeyUnstable => {
                "PatchKeyUnstable: a patch token is duplicated for one object"
            }
        }
    }
}

impl fmt::Display for Error {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.message())
    }
}

struct Catalogs {
    source_by_id: BTreeMap<u64, usize>,
    operation_by_id: BTreeMap<u64, usize>,
    object_by_id: BTreeMap<u64, usize>,
    parents: Vec<Vec<usize>>,
}

/// Validate one immutable provenance table against the caller's current view.
///
/// Rules run in a stable order: context, catalog identity and DAG, bindings,
/// shared pools, support tables and predecessors, then mapping coverage and
/// attribution. Catalogs may be unsorted, but every referenced run is checked.
/// The inputs are borrowed and never modified.
///
/// # Errors
///
/// Returns [`Error::ProvenanceStale`] for a mismatched current or future historical
/// revision, [`Error::ProvenanceUnattributable`] for absent required coverage or
/// contributors, [`Error::PatchKeyUnstable`] for ambiguous patch tokens, and
/// [`Error::ProvenanceConflict`] for every other structural conflict.
pub fn validate_provenance(request: &ValidationRequest<'_>) -> Result<(), Error> {
    validate_context(request)?;
    let catalogs = build_catalogs(&request.table)?;
    validate_bindings(&request.table, &catalogs)?;
    validate_source_binding_refs(&request.table)?;
    validate_pools(&request.table, &catalogs)?;
    let support_by_id = validate_support_tables(request)?;
    validate_predecessors(&request.table, &support_by_id)?;
    validate_mappings(request, &catalogs, &support_by_id)
}
fn validate_context(request: &ValidationRequest<'_>) -> Result<(), Error> {
    let context = request.table.context;
    if context.schema_version != SCHEMA_VERSION {
        return Err(Error::ProvenanceConflict);
    }
    if is_zero_bytes(&context.session_uuid)
        || is_zero_bytes(&request.expected_session)
        || context.session_uuid != request.expected_session
    {
        return Err(Error::ProvenanceConflict);
    }
    if context.revision != request.expected_revision {
        return Err(Error::ProvenanceStale);
    }
    if context.table_id == 0 {
        return Err(Error::ProvenanceConflict);
    }
    Ok(())
}

fn build_catalogs(table: &TableView<'_>) -> Result<Catalogs, Error> {
    let mut all_ids = BTreeSet::new();
    let mut source_by_id = BTreeMap::new();
    for (index, record) in table.sources.iter().enumerate() {
        if record.id.0 == 0 || !all_ids.insert(record.id.0) {
            return Err(Error::ProvenanceConflict);
        }
        source_by_id.insert(record.id.0, index);
    }

    let mut operation_by_id = BTreeMap::new();
    for (index, record) in table.operations.iter().enumerate() {
        if record.id.0 == 0 || !all_ids.insert(record.id.0) {
            return Err(Error::ProvenanceConflict);
        }
        operation_by_id.insert(record.id.0, index);
    }

    let mut object_by_id = BTreeMap::new();
    for (index, record) in table.objects.iter().enumerate() {
        if record.id.0 == 0 || !all_ids.insert(record.id.0) {
            return Err(Error::ProvenanceConflict);
        }
        object_by_id.insert(record.id.0, index);
    }

    for object in table.objects {
        if !operation_by_id.contains_key(&object.operation.0) {
            return Err(Error::ProvenanceConflict);
        }
    }

    let parents = build_operation_graph(table, &operation_by_id)?;
    detect_cycle(&parents)?;

    Ok(Catalogs {
        source_by_id,
        operation_by_id,
        object_by_id,
        parents,
    })
}

fn build_operation_graph(
    table: &TableView<'_>,
    operation_by_id: &BTreeMap<u64, usize>,
) -> Result<Vec<Vec<usize>>, Error> {
    let mut parents = vec![Vec::new(); table.operations.len()];
    for (index, record) in table.operations.iter().enumerate() {
        let references = range_slice(table.operation_ids, record.parents)?;
        let mut seen = BTreeSet::new();
        for reference in references {
            let parent_index = *operation_by_id
                .get(&reference.0)
                .ok_or(Error::ProvenanceConflict)?;
            if !seen.insert(reference.0) {
                return Err(Error::ProvenanceConflict);
            }
            let entry = parents.get_mut(index).ok_or(Error::ProvenanceConflict)?;
            entry.push(parent_index);
        }
    }
    Ok(parents)
}

fn detect_cycle(parents: &[Vec<usize>]) -> Result<(), Error> {
    let mut children = vec![Vec::new(); parents.len()];
    let mut remaining_parents = vec![0usize; parents.len()];
    for (child, parent_list) in parents.iter().enumerate() {
        let child_count = remaining_parents
            .get_mut(child)
            .ok_or(Error::ProvenanceConflict)?;
        for &parent in parent_list {
            *child_count = child_count.saturating_add(1);
            let siblings = children.get_mut(parent).ok_or(Error::ProvenanceConflict)?;
            siblings.push(child);
        }
    }

    let mut ready: VecDeque<usize> = remaining_parents
        .iter()
        .enumerate()
        .filter_map(|(index, &count)| (count == 0).then_some(index))
        .collect();
    let mut visited = 0usize;
    while let Some(node) = ready.pop_front() {
        visited = visited.saturating_add(1);
        let child_list = children.get(node).ok_or(Error::ProvenanceConflict)?;
        for &child in child_list {
            let count = remaining_parents
                .get_mut(child)
                .ok_or(Error::ProvenanceConflict)?;
            *count = count.saturating_sub(1);
            if *count == 0 {
                ready.push_back(child);
            }
        }
    }
    if visited == parents.len() {
        Ok(())
    } else {
        Err(Error::ProvenanceConflict)
    }
}

fn validate_bindings(table: &TableView<'_>, catalogs: &Catalogs) -> Result<(), Error> {
    let mut identities = BTreeSet::new();
    for binding in table.bindings {
        if binding.logical_kind != LOGICAL_SOURCE
            && binding.logical_kind != LOGICAL_OPERATION
            && binding.logical_kind != LOGICAL_OBJECT
        {
            return Err(Error::ProvenanceConflict);
        }
        if binding.logical_id == 0 || !logical_id_resolves(binding, catalogs) {
            return Err(Error::ProvenanceConflict);
        }
        if !is_canonical_native_guid(&binding.native_guid) {
            return Err(Error::ProvenanceConflict);
        }
        validate_metadata(binding)?;
        let role = role_bytes(table, binding.role)?;
        if binding.revision > table.context.revision {
            return Err(Error::ProvenanceStale);
        }
        let identity = (
            binding.logical_kind,
            binding.logical_id,
            binding.native_guid,
            role,
        );
        if !identities.insert(identity) {
            return Err(Error::ProvenanceConflict);
        }
    }
    Ok(())
}

fn logical_id_resolves(binding: &BindingRecord, catalogs: &Catalogs) -> bool {
    match binding.logical_kind {
        LOGICAL_SOURCE => catalogs.source_by_id.contains_key(&binding.logical_id),
        LOGICAL_OPERATION => catalogs.operation_by_id.contains_key(&binding.logical_id),
        LOGICAL_OBJECT => catalogs.object_by_id.contains_key(&binding.logical_id),
        _ => false,
    }
}

fn validate_metadata(binding: &BindingRecord) -> Result<(), Error> {
    match binding.metadata_present {
        0 if is_zero_bytes(&binding.metadata_uuid) => Ok(()),
        1 if !is_zero_bytes(&binding.metadata_uuid) => Ok(()),
        _ => Err(Error::ProvenanceConflict),
    }
}

fn role_bytes<'a>(table: &'a TableView<'_>, range: Range) -> Result<&'a [u8], Error> {
    let role = range_slice(table.role_bytes, range)?;
    if role.is_empty() || role.contains(&0) || std::str::from_utf8(role).is_err() {
        return Err(Error::ProvenanceConflict);
    }
    Ok(role)
}

fn validate_source_binding_refs(table: &TableView<'_>) -> Result<(), Error> {
    for reference in table.binding_refs {
        let binding_index = usize::try_from(*reference).map_err(|_| Error::ProvenanceConflict)?;
        if table.bindings.get(binding_index).is_none() {
            return Err(Error::ProvenanceConflict);
        }
    }

    for source in table.sources {
        let references = range_slice(table.binding_refs, source.bindings)?;
        let mut seen = BTreeSet::new();
        for reference in references {
            if !seen.insert(*reference) {
                return Err(Error::ProvenanceConflict);
            }
            let binding_index =
                usize::try_from(*reference).map_err(|_| Error::ProvenanceConflict)?;
            let binding = table
                .bindings
                .get(binding_index)
                .ok_or(Error::ProvenanceConflict)?;
            if binding.logical_kind != LOGICAL_SOURCE || binding.logical_id != source.id.0 {
                return Err(Error::ProvenanceConflict);
            }
        }
    }
    Ok(())
}

fn validate_pools(table: &TableView<'_>, catalogs: &Catalogs) -> Result<(), Error> {
    for source_id in table.source_ids {
        if source_id.0 == 0 || !catalogs.source_by_id.contains_key(&source_id.0) {
            return Err(Error::ProvenanceConflict);
        }
    }
    for operation_id in table.operation_ids {
        if operation_id.0 == 0 || !catalogs.operation_by_id.contains_key(&operation_id.0) {
            return Err(Error::ProvenanceConflict);
        }
    }
    Ok(())
}

fn validate_support_tables(request: &ValidationRequest<'_>) -> Result<BTreeMap<u64, usize>, Error> {
    let mut support_by_id = BTreeMap::new();
    for (index, support) in request.table.support_tables.iter().enumerate() {
        validate_support_context(request, support)?;
        if support_by_id
            .insert(support.context.table_id, index)
            .is_some()
        {
            return Err(Error::ProvenanceConflict);
        }
    }
    Ok(support_by_id)
}

fn validate_support_context(
    request: &ValidationRequest<'_>,
    support: &SupportTable,
) -> Result<(), Error> {
    let context = support.context;
    if context.schema_version != SCHEMA_VERSION {
        return Err(Error::ProvenanceConflict);
    }
    if is_zero_bytes(&context.session_uuid)
        || context.session_uuid != request.table.context.session_uuid
    {
        return Err(Error::ProvenanceConflict);
    }
    if context.table_id == 0 || context.table_id == request.table.context.table_id {
        return Err(Error::ProvenanceConflict);
    }
    match support.historical {
        0 if context.revision == request.expected_revision => Ok(()),
        1 if context.revision <= request.expected_revision => Ok(()),
        0 | 1 => Err(Error::ProvenanceStale),
        _ => Err(Error::ProvenanceConflict),
    }
}

fn validate_predecessors(
    table: &TableView<'_>,
    support_by_id: &BTreeMap<u64, usize>,
) -> Result<(), Error> {
    for predecessor in table.predecessors {
        let support_index = *support_by_id
            .get(&predecessor.table_id)
            .ok_or(Error::ProvenanceConflict)?;
        let support = table
            .support_tables
            .get(support_index)
            .ok_or(Error::ProvenanceConflict)?;
        let count =
            entity_count(support.counts, predecessor.domain).ok_or(Error::ProvenanceConflict)?;
        if predecessor.entity_index >= count {
            return Err(Error::ProvenanceConflict);
        }
    }
    Ok(())
}

fn validate_mappings(
    request: &ValidationRequest<'_>,
    catalogs: &Catalogs,
    support_by_id: &BTreeMap<u64, usize>,
) -> Result<(), Error> {
    if request.domains == 0 || request.domains & !DOMAIN_MASK_ALL != 0 {
        return Err(Error::ProvenanceConflict);
    }
    for domain in DOMAINS {
        let mask = domain_mask(domain).ok_or(Error::ProvenanceConflict)?;
        let count = entity_count(request.counts, domain).ok_or(Error::ProvenanceConflict)?;
        if request.domains & mask == 0 && count != 0 {
            return Err(Error::ProvenanceConflict);
        }
    }

    let mut output_indexes: Vec<BTreeSet<u64>> = vec![BTreeSet::new(); DOMAINS.len()];
    let mut patch_tokens = BTreeSet::new();
    for mapping in request.table.mappings {
        let domain_index = domain_index(mapping.domain).ok_or(Error::ProvenanceConflict)?;
        let mask = domain_mask(mapping.domain).ok_or(Error::ProvenanceConflict)?;
        if request.domains & mask == 0 {
            return Err(Error::ProvenanceConflict);
        }
        let count =
            entity_count(request.counts, mapping.domain).ok_or(Error::ProvenanceConflict)?;
        if mapping.output_index >= count {
            return Err(Error::ProvenanceConflict);
        }
        let indexes = output_indexes
            .get_mut(domain_index)
            .ok_or(Error::ProvenanceConflict)?;
        if !indexes.insert(mapping.output_index) {
            return Err(Error::ProvenanceConflict);
        }

        let operation_indexes = validate_mapping_contributors(request, catalogs, mapping)?;
        validate_mapping_object(request, catalogs, mapping, &operation_indexes)?;
        validate_mapping_predecessors(request, mapping, support_by_id)?;
        validate_patch_key(mapping, &mut patch_tokens)?;
    }

    for (domain_index, domain) in DOMAINS.iter().enumerate() {
        let mask = domain_mask(*domain).ok_or(Error::ProvenanceConflict)?;
        if request.domains & mask == 0 {
            continue;
        }
        let indexes = output_indexes
            .get(domain_index)
            .ok_or(Error::ProvenanceConflict)?;
        let actual = u64::try_from(indexes.len()).map_err(|_| Error::ProvenanceConflict)?;
        let expected = entity_count(request.counts, *domain).ok_or(Error::ProvenanceConflict)?;
        if actual != expected {
            return Err(Error::ProvenanceUnattributable);
        }
    }
    Ok(())
}

fn validate_mapping_contributors(
    request: &ValidationRequest<'_>,
    catalogs: &Catalogs,
    mapping: &MappingRecord,
) -> Result<Vec<usize>, Error> {
    let source_ids = range_slice(request.table.source_ids, mapping.sources)?;
    if source_ids.is_empty() {
        return Err(Error::ProvenanceUnattributable);
    }
    validate_strictly_ascending(source_ids.iter().map(|id| id.0))?;
    for source_id in source_ids {
        if source_id.0 == 0 || !catalogs.source_by_id.contains_key(&source_id.0) {
            return Err(Error::ProvenanceConflict);
        }
    }

    let operation_ids = range_slice(request.table.operation_ids, mapping.operations)?;
    if operation_ids.is_empty() {
        return Err(Error::ProvenanceUnattributable);
    }
    validate_strictly_ascending(operation_ids.iter().map(|id| id.0))?;
    operation_ids
        .iter()
        .map(|operation_id| {
            if operation_id.0 == 0 {
                return Err(Error::ProvenanceConflict);
            }
            catalogs
                .operation_by_id
                .get(&operation_id.0)
                .copied()
                .ok_or(Error::ProvenanceConflict)
        })
        .collect()
}

fn validate_mapping_object(
    request: &ValidationRequest<'_>,
    catalogs: &Catalogs,
    mapping: &MappingRecord,
    operation_indexes: &[usize],
) -> Result<(), Error> {
    if mapping.object.0 == 0 {
        if request.require_object || mapping.domain == DOMAIN_PATCH {
            return Err(Error::ProvenanceUnattributable);
        }
        return Ok(());
    }

    let object_index = *catalogs
        .object_by_id
        .get(&mapping.object.0)
        .ok_or(Error::ProvenanceConflict)?;
    let object = request
        .table
        .objects
        .get(object_index)
        .ok_or(Error::ProvenanceConflict)?;
    let owner_index = *catalogs
        .operation_by_id
        .get(&object.operation.0)
        .ok_or(Error::ProvenanceConflict)?;
    if !operations_reach(&catalogs.parents, operation_indexes, owner_index) {
        return Err(Error::ProvenanceUnattributable);
    }
    Ok(())
}

fn validate_mapping_predecessors(
    request: &ValidationRequest<'_>,
    mapping: &MappingRecord,
    support_by_id: &BTreeMap<u64, usize>,
) -> Result<(), Error> {
    let predecessors = range_slice(request.table.predecessors, mapping.predecessors)?;
    let mut locators = BTreeSet::new();
    for predecessor in predecessors {
        if !support_by_id.contains_key(&predecessor.table_id)
            || domain_index(predecessor.domain).is_none()
        {
            return Err(Error::ProvenanceConflict);
        }
        let locator = (
            predecessor.table_id,
            predecessor.domain,
            predecessor.entity_index,
        );
        if !locators.insert(locator) {
            return Err(Error::ProvenanceConflict);
        }
    }

    match mapping.kind {
        KIND_EXACT => {
            if predecessors.is_empty() {
                return Err(Error::ProvenanceUnattributable);
            }
            if predecessors.len() != 1 {
                return Err(Error::ProvenanceConflict);
            }
            let predecessor = predecessors.first().ok_or(Error::ProvenanceConflict)?;
            if predecessor.domain != mapping.domain {
                return Err(Error::ProvenanceConflict);
            }
        }
        KIND_INHERITED | KIND_SPLIT => {
            if predecessors.is_empty() {
                return Err(Error::ProvenanceUnattributable);
            }
        }
        KIND_MERGED => {
            if locators.len() < 2 {
                return Err(Error::ProvenanceUnattributable);
            }
        }
        KIND_GENERATED => {}
        _ => return Err(Error::ProvenanceConflict),
    }
    Ok(())
}

fn validate_patch_key(
    mapping: &MappingRecord,
    patch_tokens: &mut BTreeSet<(u64, u64)>,
) -> Result<(), Error> {
    if mapping.domain != DOMAIN_PATCH {
        // Non-patch entities may refer to an object's persistent output patch.
        // Uniqueness applies to patch rows, not every member of that patch.
        if mapping.patch_key != 0 && mapping.object.0 == 0 {
            return Err(Error::ProvenanceConflict);
        }
        return Ok(());
    }
    if mapping.object.0 == 0 {
        return Err(Error::ProvenanceUnattributable);
    }
    if mapping.patch_key == 0 {
        return Err(Error::ProvenanceConflict);
    }
    if !patch_tokens.insert((mapping.object.0, mapping.patch_key)) {
        return Err(Error::PatchKeyUnstable);
    }
    Ok(())
}

fn operations_reach(parents: &[Vec<usize>], starts: &[usize], target: usize) -> bool {
    let mut visited = BTreeSet::new();
    let mut stack = Vec::new();
    for &start in starts {
        if start == target {
            return true;
        }
        if visited.insert(start) {
            stack.push(start);
        }
    }
    while let Some(node) = stack.pop() {
        let Some(parent_list) = parents.get(node) else {
            continue;
        };
        for &parent in parent_list {
            if parent == target {
                return true;
            }
            if visited.insert(parent) {
                stack.push(parent);
            }
        }
    }
    false
}

fn validate_strictly_ascending<I>(ids: I) -> Result<(), Error>
where
    I: IntoIterator<Item = u64>,
{
    let mut previous = None;
    for id in ids {
        if let Some(previous_id) = previous
            && id <= previous_id
        {
            return Err(Error::ProvenanceConflict);
        }
        previous = Some(id);
    }
    Ok(())
}

fn range_slice<T>(slice: &[T], range: Range) -> Result<&[T], Error> {
    let start = usize::try_from(range.offset).map_err(|_| Error::ProvenanceConflict)?;
    let count = usize::try_from(range.count).map_err(|_| Error::ProvenanceConflict)?;
    let end = start.checked_add(count).ok_or(Error::ProvenanceConflict)?;
    slice.get(start..end).ok_or(Error::ProvenanceConflict)
}

const fn domain_index(domain: u32) -> Option<usize> {
    match domain {
        DOMAIN_VERTEX => Some(0),
        DOMAIN_EDGE => Some(1),
        DOMAIN_FACE => Some(2),
        DOMAIN_PATCH => Some(3),
        _ => None,
    }
}

const fn domain_mask(domain: u32) -> Option<u32> {
    match domain {
        DOMAIN_VERTEX => Some(1),
        DOMAIN_EDGE => Some(1 << 1),
        DOMAIN_FACE => Some(1 << 2),
        DOMAIN_PATCH => Some(1 << 3),
        _ => None,
    }
}

const fn entity_count(counts: EntityCounts, domain: u32) -> Option<u64> {
    match domain {
        DOMAIN_VERTEX => Some(counts.vertices),
        DOMAIN_EDGE => Some(counts.edges),
        DOMAIN_FACE => Some(counts.faces),
        DOMAIN_PATCH => Some(counts.patches),
        _ => None,
    }
}

fn is_zero_bytes<const N: usize>(bytes: &[u8; N]) -> bool {
    bytes.iter().all(|&byte| byte == 0)
}

fn is_upper_hex(byte: u8) -> bool {
    byte.is_ascii_digit() || (b'A'..=b'F').contains(&byte)
}

fn is_canonical_native_guid(guid: &[u8; 36]) -> bool {
    let mut has_nonzero = false;
    for (index, &byte) in guid.iter().enumerate() {
        if DASH_POSITIONS.contains(&index) {
            if byte != b'-' {
                return false;
            }
        } else if !is_upper_hex(byte) {
            return false;
        } else if byte != b'0' {
            has_nonzero = true;
        }
    }
    has_nonzero
}
