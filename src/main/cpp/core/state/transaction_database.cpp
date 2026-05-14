#include "transaction_database_internal.h"

TransactionDatabase::TransactionDatabase(const ReqPackConfig& config) : config(config) {}

TransactionDatabase::~TransactionDatabase() {
	std::lock_guard<std::mutex> lock(this->mutex);
	if (this->initialized && this->env != nullptr) {
		mdb_dbi_close(this->env, this->dbi);
		mdb_env_close(this->env);
		this->env = nullptr;
		this->initialized = false;
	}
}

bool TransactionDatabase::ensureReady() const {
	return this->initStorage();
}
