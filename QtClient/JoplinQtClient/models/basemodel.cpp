#include "basemodel.h"

#include "models/change.h"
#include "database.h"
#include "uuid.h"

using namespace jop;

QMap<int, QVector<BaseModel::Field>> BaseModel::tableFields_;
QHash<QString, QVariant> BaseModel::cache_;

BaseModel::BaseModel() {}

BaseModel::BaseModel(const QSqlQuery &query) {
	loadSqlQuery(query);
}

QStringList BaseModel::changedFields() const {
	QStringList output;
	for (QHash<QString, bool>::const_iterator it = changedFields_.begin(); it != changedFields_.end(); ++it) {
		output.push_back(it.key());
	}
	return output;
}

int BaseModel::count(Table table) {
	QString t = BaseModel::tableName(table);
	QString k = QString("%1:count").arg(t);
	QVariant r = BaseModel::cacheGet(k);
	if (r.isValid()) return r.toInt();

	QSqlQuery q("SELECT count(*) AS row_count FROM " + t);
	jop::db().execQuery(q);
	q.next();
	int output = q.value(0).toInt();
	BaseModel::cacheSet(k, QVariant(output));
	return output;
}

bool BaseModel::save() {
	bool isNew = this->isNew();

	if (!changedFields_.size() && !isNew) return true;

	QStringList fields = changedFields();

	QMap<QString, QVariant> values;

	foreach (QString field, fields) {
		values[field] = value(field).toQVariant();
	}

	// If it's a new entry and the ID is a UUID, we need to create this
	// ID now. If the ID is an INT, it will be automatically set by
	// SQLite.
	if (isNew && primaryKeyIsUuid()) {
		values[primaryKey()] = uuid::createUuid();
	}

	// Update created_time and updated_time if needed. If updated_time
	// has already been updated (maybe manually by the user), don't
	// automatically update it.
	if (isNew) {
		if (BaseModel::hasField(table(), "created_time")) {
			values["created_time"] = (int)(QDateTime::currentMSecsSinceEpoch() / 1000);
		}
	} else {
		if (!values.contains("updated_time")) {
			if (BaseModel::hasField(table(), "updated_time")) {
				values["updated_time"] = (int)(QDateTime::currentMSecsSinceEpoch() / 1000);
			}
		}
	}

	changedFields_.clear();

	const QString& tableName = BaseModel::tableName(table());

	if (isNew) {
		cacheDelete(QString("%1:count").arg(tableName));
	}

	bool output = false;

	jop::db().transaction();

	if (isNew) {
		QSqlQuery q = jop::db().buildSqlQuery(Database::Insert, tableName, values);
		jop::db().execQuery(q);
		output = jop::db().errorCheck(q);
		if (output) setValue("id", values["id"]);
	} else {
		QSqlQuery q = jop::db().buildSqlQuery(Database::Update, tableName, values, QString("%1 = '%2'").arg(primaryKey()).arg(value("id").toString()));
		jop::db().execQuery(q);
		output = jop::db().errorCheck(q);
	}

	if (output && trackChanges()) {
		if (isNew) {
			Change change;
			change.setValue("item_id", id());
			change.setValue("item_type", table());
			change.setValue("type", Change::Create);
			change.save();
		} else {
			for (QMap<QString, QVariant>::const_iterator it = values.begin(); it != values.end(); ++it) {
				Change change;
				change.setValue("item_id", id());
				change.setValue("item_type", table());
				change.setValue("type", Change::Update);
				change.setValue("item_field", it.key());
				change.save();
			}
		}
	}

	jop::db().commit();

	return output;
}

bool BaseModel::dispose() {
	const QString& tableName = BaseModel::tableName(table());
	QSqlQuery q(jop::db().database());
	q.prepare("DELETE FROM " + tableName + " WHERE " + primaryKey() + " = :id");
	q.bindValue(":id", id().toString());
	jop::db().execQuery(q);

	bool output = jop::db().errorCheck(q);

	if (output) cacheDelete(QString("%1:count").arg(tableName));

	if (output && trackChanges()) {
		Change change;
		change.setValue("item_id", id());
		change.setValue("item_type", table());
		change.setValue("type", Change::Delete);
		change.save();
	}

	return output;
}

Table BaseModel::table() const {
	qCritical() << "BaseModel::table() must be overriden";
	return jop::UndefinedTable;
}

QString BaseModel::primaryKey() const {
	return "id";
}

bool BaseModel::primaryKeyIsUuid() const {
	return false;
}

bool BaseModel::trackChanges() const {
	return false;
}

bool BaseModel::isNew() const {
	return !valueIsSet(primaryKey());
}

BaseModel::Field createField(const QString& name, QMetaType::Type type) {
	BaseModel::Field c;
	c.name = name;
	c.type = type;
	return c;
}

QVector<BaseModel::Field> BaseModel::tableFields(jop::Table table) {
	if (BaseModel::tableFields_.contains(table)) return BaseModel::tableFields_[table];

	QVector<BaseModel::Field> output;

	if (table == jop::FoldersTable) {
		output.push_back(createField("id", QMetaType::QString ));
		output.push_back(createField("title", QMetaType::QString ));
		output.push_back(createField("created_time", QMetaType::Int ));
		output.push_back(createField("updated_time", QMetaType::Int ));
	} else if (table == jop::ChangesTable) {
		output.push_back(createField("id", QMetaType::Int ));
		output.push_back(createField("type", QMetaType::Int ));
		output.push_back(createField("item_id", QMetaType::QString ));
		output.push_back(createField("item_type", QMetaType::Int ));
		output.push_back(createField("item_field", QMetaType::QString ));
	}

	BaseModel::tableFields_[table] = output;
	return output;
}

bool BaseModel::hasField(jop::Table table, const QString &name) {
	QVector<BaseModel::Field> fields = tableFields(table);
	foreach (Field field, fields) {
		if (field.name == name) return true;
	}
	return false;
}

QStringList BaseModel::tableFieldNames(Table table) {
	QVector<BaseModel::Field> fields = BaseModel::tableFields(table);
	QStringList output;
	foreach (BaseModel::Field field, fields) {
		output.push_back(field.name);
	}
	return output;
}

bool BaseModel::isValidFieldName(Table table, const QString &name) {
	QVector<BaseModel::Field> fields = BaseModel::tableFields(table);
	foreach (BaseModel::Field col, fields) {
		if (col.name == name) return true;
	}
	return false;
}

void BaseModel::loadSqlQuery(const QSqlQuery &query) {
	values_.clear();
	QSqlRecord record = query.record();
	QVector<BaseModel::Field> fields = BaseModel::tableFields(table());

	foreach (BaseModel::Field field, fields) {
		int idx = record.indexOf(field.name);
		if (idx < 0) {
			qCritical() << "Cannot find field" << field.name;
			continue;
		}

		if (field.type == QMetaType::QString) {
			values_.insert(field.name, Value(query.value(idx).toString()));
		} else if (field.type == QMetaType::Int) {
			values_.insert(field.name, Value(query.value(idx).toInt()));
		} else {
			qCritical() << "Unsupported value type" << field.name;
		}
	}

	changedFields_.clear();
}

QHash<QString, BaseModel::Value> BaseModel::values() const {
	return values_;
}

BaseModel::Value BaseModel::value(const QString &name) const {
	if (!valueIsSet(name)) {
		qCritical() << "Value does not exist" << name;
		return Value();
	}
	return values_[name];
}

bool BaseModel::valueIsSet(const QString &name) const {
	return values_.contains(name);
}

void BaseModel::setValue(const QString &name, const BaseModel::Value &value) {
	if (!values_.contains(name)) {
		values_.insert(name, value);
		changedFields_.insert(name, true);
	} else {
		Value& v = values_[name];
		if (v.isEqual(value)) return;
		values_.insert(name, value);
		changedFields_.insert(name, true);
	}
}

void BaseModel::setValue(const QString &name, int value) {
	setValue(name, Value(value));
}

BaseModel::Value BaseModel::id() const {
	if (!valueIsSet(primaryKey())) return QVariant();
	return value(primaryKey());
}

QString BaseModel::tableName(Table t) {
	if (t == jop::FoldersTable) return "folders";
	if (t == jop::NotesTable) return "notes";
	if (t == jop::ChangesTable) return "changes";
	return "UNDEFINED";
}

QVariant BaseModel::cacheGet(const QString &key) {
	if (!BaseModel::cache_.contains(key)) return QVariant();
	return cache_[key];
}

void BaseModel::cacheSet(const QString &key, const QVariant &value) {
	BaseModel::cache_[key] = value;
}

void BaseModel::cacheDelete(const QString &key) {
	BaseModel::cache_.remove(key);
}

void BaseModel::setValue(const QString &name, const QString &value) {
	setValue(name, Value(value));
}

void BaseModel::setValue(const QString& name, const QVariant& value) {
	setValue(name, Value(value));
}

BaseModel::Value::Value() {}

BaseModel::Value::Value(const QString &v) {
	type_ = QMetaType::QString;
	stringValue_ = v;
}

BaseModel::Value::Value(int v) {
	type_ = QMetaType::Int;
	intValue_ = v;
}

BaseModel::Value::Value(const QVariant &v) {
	type_ = (QMetaType::Type)v.type();
	if (type_ == QMetaType::QString) {
		stringValue_ = v.toString();
	} else if (type_ == QMetaType::Int) {
		intValue_ = v.toInt();
	} else {
		// Creates an invalid Value, which is what we want
	}
}

int BaseModel::Value::toInt() const {
	return intValue_;
}

QString BaseModel::Value::toString() const {
	return stringValue_;
}

QVariant BaseModel::Value::toQVariant() const {
	QMetaType::Type t = type();
	if (t == QMetaType::QString) return QVariant(toString());
	if (t == QMetaType::Int) return QVariant(toInt());
	return QVariant();
}

QMetaType::Type BaseModel::Value::type() const {
	return type_;
}

bool BaseModel::Value::isValid() const {
	return type_ > 0;
}

bool BaseModel::Value::isEqual(const BaseModel::Value &v) const {
	QMetaType::Type type = v.type();
	if (this->type() != type) return false;
	if (type == QMetaType::QString) return toString() == v.toString();
	if (type == QMetaType::Int) return toInt() == v.toInt();

	qCritical() << "Unreachable";
	return false;
}