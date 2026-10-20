#include "slowmail/core/store.hpp"

namespace slowmail::core {

Transaction::Transaction(Store& s) : store_(s), begin_status_(s.begin()) {
    // If begin() failed there is nothing to roll back (rolling back would hit an enclosing
    // transaction). commit() reports the original begin() error.
    if (!begin_status_.ok()) {
        done_ = true;
    }
}

Transaction::~Transaction() {
    if (!done_) {
        (void)store_.rollback();
    }
}

Status Transaction::commit() {
    if (!begin_status_.ok()) return begin_status_;
    if (done_) {
        Error e;
        e.kind = ErrorKind::Invalid;
        e.message = "transaction already finished";
        return Status(e);
    }
    Status st = store_.commit();
    if (st.ok()) {
        done_ = true;
    }
    // On failure the destructor rolls back.
    return st;
}

} // namespace slowmail::core
