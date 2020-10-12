/******************************************************************************
 *
 * Copyright (c) 2017, the Perspective Authors.
 *
 * This file is part of the Perspective library, distributed under the terms of
 * the Apache License 2.0.  The full license can be found in the LICENSE file.
 *
 */

#include <perspective/first.h>
#include <perspective/context_base.h>
#include <perspective/get_data_extents.h>
#include <perspective/context_unit.h>
#include <perspective/flat_traversal.h>
#include <perspective/sym_table.h>
#include <perspective/logtime.h>
#include <perspective/filter_utils.h>

namespace perspective {

t_ctxunit::t_ctxunit() {}

t_ctxunit::t_ctxunit(const t_schema& schema)
    : t_ctxbase<t_ctxunit>(schema, t_config())
    , m_has_delta(false)
{}

t_ctxunit::~t_ctxunit() {}

void
t_ctxunit::init() {
    m_init = true;
}


std::string
t_ctxunit::repr() const {
    std::stringstream ss;
    ss << "t_ctxunit<" << this << ">";
    return ss.str();
}

void
t_ctxunit::step_begin() {
    if (!m_init)
        return;

    m_delta_pkeys.clear();
    m_rows_changed = false;
    m_columns_changed = false;
}

void
t_ctxunit::step_end() {}

t_index
t_ctxunit::get_row_count() const {
    return m_gstate->num_rows();
}

t_index
t_ctxunit::get_column_count() const {
    return m_gstate->num_columns();
}

/**
 * @brief Given a start/end row and column index, return the underlying data for the requested
 * subset.
 *
 * @param start_row
 * @param end_row
 * @param start_col
 * @param end_col
 * @return std::vector<t_tscalar>
 */
std::vector<t_tscalar>
t_ctxunit::get_data(t_index start_row, t_index end_row, t_index start_col, t_index end_col) const {
    t_uindex ctx_nrows = get_row_count();
    t_uindex ctx_ncols = get_column_count();
    auto ext = sanitize_get_data_extents(
        ctx_nrows, ctx_ncols, start_row, end_row, start_col, end_col);

    t_index num_rows = ext.m_erow - ext.m_srow;
    t_index stride = ext.m_ecol - ext.m_scol;
    std::vector<t_tscalar> values(num_rows * stride);

    auto none = mknone();

    const std::vector<std::string>& columns = m_schema.columns();

    for (t_index cidx = ext.m_scol; cidx < ext.m_ecol; ++cidx) {
        std::vector<t_tscalar> out_data(num_rows);

        m_gstate->read_column(columns[cidx], start_row, end_row, out_data);

        for (t_index ridx = ext.m_srow; ridx < ext.m_erow; ++ridx) {
            auto v = out_data[ridx - ext.m_srow];

            // todo: fix null handling
            if (!v.is_valid())
                v.set(none);

            values[(ridx - ext.m_srow) * stride + (cidx - ext.m_scol)] = v;
        }
    }

    return values;
}

/**
 * @brief Given a vector of row indices, which may not be contiguous, return the underlying data
 * for these rows.
 *
 * @param rows a vector of row indices
 * @return std::vector<t_tscalar> a vector of scalars containing the underlying data
 */
std::vector<t_tscalar>
t_ctxunit::get_data(const std::vector<t_uindex>& rows) const {
    t_uindex stride = get_column_count();
    std::vector<t_tscalar> values(rows.size() * stride);

    auto none = mknone();

    const std::vector<std::string>& columns = m_schema.columns();

    for (t_uindex cidx = 0; cidx < stride; ++cidx) {
        std::vector<t_tscalar> out_data(rows.size());
        m_gstate->read_column(columns[cidx], rows, out_data);

        for (t_uindex ridx = 0; ridx < rows.size(); ++ridx) {
            auto v = out_data[ridx];

            if (!v.is_valid())
                v.set(none);

            values[(ridx) * stride + (cidx)] = v;
        }
    }

    return values;
}

std::vector<t_tscalar>
t_ctxunit::get_data(const std::vector<t_tscalar>& pkeys) const {
    t_uindex stride = get_column_count();
    std::vector<t_tscalar> values(pkeys.size() * stride);

    auto none = mknone();

    const std::vector<std::string>& columns = m_schema.columns();

    for (t_uindex cidx = 0; cidx < stride; ++cidx) {
        std::vector<t_tscalar> out_data(pkeys.size());
        m_gstate->read_column(columns[cidx], pkeys, out_data);

        for (t_uindex ridx = 0; ridx < pkeys.size(); ++ridx) {
            auto v = out_data[ridx];

            if (!v.is_valid())
                v.set(none);

            values[(ridx) * stride + (cidx)] = v;
        }
    }

    return values;
}

std::string
t_ctxunit::get_column_name(t_index idx) const {
    const std::vector<std::string>& columns = m_schema.columns();

    if (idx >= columns.size()) {
       return "";
    }

    return columns[idx];
}

std::vector<t_tscalar>
t_ctxunit::get_pkeys(const std::vector<std::pair<t_uindex, t_uindex>>& cells) const {
    PSP_COMPLAIN_AND_ABORT("Not implemented yet");
}

/**
 * @brief Returns a `t_rowdelta` struct containing data from updated rows
 * and the updated row indices.
 *
 * @return t_rowdelta
 */
t_rowdelta
t_ctxunit::get_row_delta() {
    bool rows_changed = m_rows_changed;
    tsl::hopscotch_set<t_tscalar> pkeys = get_delta_pkeys();
    std::vector<t_tscalar> pkey_vector(pkeys.size());

    // Copy from set to vector for get_data, which only (for now) takes a
    // vector of primary keys.
    std::copy(pkeys.begin(), pkeys.end(), pkey_vector.begin());

    std::vector<t_tscalar> data = get_data(pkey_vector);
    t_rowdelta rval(rows_changed, pkey_vector.size(), data);
    clear_deltas();

    return rval;
}

const tsl::hopscotch_set<t_tscalar>&
t_ctxunit::get_delta_pkeys() const {
    return m_delta_pkeys;
}

std::vector<std::string>
t_ctxunit::get_column_names() const {
    return m_schema.columns();
}

void
t_ctxunit::reset() {
    m_has_delta = false;
}

bool
t_ctxunit::get_deltas_enabled() const {
    return true;
}

t_index
t_ctxunit::sidedness() const {
    return 0;
}

/**
 * @brief Notify the context with new data when the `t_gstate` master table is
 * not empty, and being updated with new data.
 * 
 * @param flattened 
 * @param delta 
 * @param prev 
 * @param curr 
 * @param transitions 
 * @param existed 
 */
void
t_ctxunit::notify(const t_data_table& flattened, const t_data_table& delta,
    const t_data_table& prev, const t_data_table& curr, const t_data_table& transitions,
    const t_data_table& existed) {
    t_uindex nrecs = flattened.size();

    std::shared_ptr<const t_column> pkey_sptr = flattened.get_const_column("psp_pkey");
    std::shared_ptr<const t_column> op_sptr = flattened.get_const_column("psp_op");
    const t_column* pkey_col = pkey_sptr.get();
    const t_column* op_col = op_sptr.get();

    bool delete_encountered = false;

    // Context does not have filters applied
    for (t_uindex idx = 0; idx < nrecs; ++idx) {
        t_tscalar pkey = m_symtable.get_interned_tscalar(pkey_col->get_scalar(idx));
        std::uint8_t op_ = *(op_col->get_nth<std::uint8_t>(idx));
        t_op op = static_cast<t_op>(op_);

        switch (op) {
            case OP_INSERT: {}
                break;
            case OP_DELETE: {
                delete_encountered = true;
            } break;
            default: { PSP_COMPLAIN_AND_ABORT("Unexpected OP"); } break;
        }

        // add the pkey for row delta
        add_delta_pkey(pkey);
    }

    m_has_delta = m_delta_pkeys.size() > 0 || delete_encountered;
}

/**
 * @brief Notify the context with new data after the `t_gstate`'s master table
 * has been updated for the first time with data.
 * 
 * @param flattened 
 */
void
t_ctxunit::notify(const t_data_table& flattened) {
    t_uindex nrecs = flattened.size();
    std::shared_ptr<const t_column> pkey_sptr = flattened.get_const_column("psp_pkey");
    const t_column* pkey_col = pkey_sptr.get();

    m_has_delta = true;

    for (t_uindex idx = 0; idx < nrecs; ++idx) {
        t_tscalar pkey = m_symtable.get_interned_tscalar(pkey_col->get_scalar(idx));

        // Add primary key to track row delta
        add_delta_pkey(pkey);
    }
}

/**
 * @brief Mark a primary key as updated by adding it to the tracking set.
 *
 * @param pkey
 */
void
t_ctxunit::add_delta_pkey(t_tscalar pkey) {
    m_delta_pkeys.insert(pkey);
}

bool
t_ctxunit::has_deltas() const {
    return m_has_delta;
}

void
t_ctxunit::pprint() const {}

t_dtype
t_ctxunit::get_column_dtype(t_uindex idx) const {
    if (idx >= static_cast<t_uindex>(get_column_count()))
        return DTYPE_NONE;

    const std::vector<std::string>& columns = m_schema.columns();
    const std::string& column_name = get_column_name(idx);

    if (!m_schema.has_column(column_name))
        return DTYPE_NONE;

    return m_schema.get_dtype(column_name);
}

void
t_ctxunit::clear_deltas() {
    m_has_delta = false;
}

} // end namespace perspective