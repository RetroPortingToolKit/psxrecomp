#pragma once
#ifdef PSX_DIAGNOSTIC_WRITER_TEST
#define RECOMP_DIAGNOSTIC_ROW_WRITER_TEST
#endif
#include <recomp_diagnostics/row_writer.hpp>
namespace psx_playtest_diagnostic {
using recomp::diagnostics::rows::Row;
using recomp::diagnostics::rows::RowColumns;
template<size_t Capacity=4096,class Record=Row>
using Writer = recomp::diagnostics::rows::Writer<Capacity,Record>;
}
