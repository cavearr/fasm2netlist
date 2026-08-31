// Minimal, self-contained JSON reader for tilegrid.json / tileconn.json /
// tile_type_*.json. No external dependency (none of this repo's vendored
// third_party libs include a JSON library) -- a small recursive-descent
// parser over std::string/std::vector/std::map is plenty for one-shot
// loading of a few fixed-schema database files.
//
// Deliberately NOT a general-purpose/standards-exhaustive JSON library:
// object key order is preserved (needed nowhere here, but harmless), numbers
// are parsed as double or int64 (whichever fits), and there is no support
// for things prjxray's own JSON never emits (comments, trailing commas,
// unicode escapes beyond \uXXXX -> UTF-8).
#pragma once
#include <cstdint>
#include <cctype>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace json {

class Value;
using Array = std::vector<Value>;
using Object = std::vector<std::pair<std::string, Value>>;  // preserves order; lookup is linear (fine for our sizes)

enum class Type { Null, Bool, Int, Double, String, Array, Object };

class Value {
       public:
	Type type = Type::Null;
	bool b = false;
	int64_t i = 0;
	double d = 0;
	std::string s;
	std::shared_ptr<Array> arr;
	std::shared_ptr<Object> obj;

	Value() = default;
	static Value makeNull() { return Value(); }

	bool isNull() const { return type == Type::Null; }
	bool isObject() const { return type == Type::Object; }
	bool isArray() const { return type == Type::Array; }
	bool isString() const { return type == Type::String; }

	const std::string& asString() const { return s; }
	int64_t asInt() const {
		if (type == Type::Int) return i;
		if (type == Type::Double) return (int64_t)d;
		throw std::runtime_error("json: not a number");
	}
	double asDouble() const {
		if (type == Type::Double) return d;
		if (type == Type::Int) return (double)i;
		throw std::runtime_error("json: not a number");
	}

	// object member access; returns a Null Value if absent (no throw) so
	// callers can chain `.get("x").get("y")` defensively.
	const Value& get(const std::string& key) const {
		static const Value kNull;
		if (type != Type::Object) return kNull;
		for (auto& kv : *obj)
			if (kv.first == key) return kv.second;
		return kNull;
	}
	bool has(const std::string& key) const { return !get(key).isNull(); }

	const Array& items() const {
		static const Array kEmpty;
		return arr ? *arr : kEmpty;
	}
	const Object& members() const {
		static const Object kEmpty;
		return obj ? *obj : kEmpty;
	}
};

namespace detail {

class Parser {
       public:
	Parser(const char* p, const char* end) : p_(p), end_(end) {}

	Value parse() {
		skipWs();
		Value v = parseValue();
		return v;
	}

       private:
	const char* p_;
	const char* end_;

	void skipWs() {
		while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) p_++;
	}
	char peek() {
		if (p_ >= end_) throw std::runtime_error("json: unexpected end of input");
		return *p_;
	}
	char next() {
		char c = peek();
		p_++;
		return c;
	}
	void expect(char c) {
		if (next() != c) throw std::runtime_error(std::string("json: expected '") + c + "'");
	}

	Value parseValue() {
		skipWs();
		char c = peek();
		if (c == '{') return parseObject();
		if (c == '[') return parseArray();
		if (c == '"') return parseString();
		if (c == 't' || c == 'f') return parseBool();
		if (c == 'n') return parseNull();
		return parseNumber();
	}

	Value parseObject() {
		expect('{');
		Value v;
		v.type = Type::Object;
		v.obj = std::make_shared<Object>();
		skipWs();
		if (peek() == '}') {
			p_++;
			return v;
		}
		while (true) {
			skipWs();
			Value key = parseString();
			skipWs();
			expect(':');
			Value val = parseValue();
			v.obj->emplace_back(key.s, std::move(val));
			skipWs();
			char c = next();
			if (c == '}') break;
			if (c != ',') throw std::runtime_error("json: expected ',' or '}'");
		}
		return v;
	}

	Value parseArray() {
		expect('[');
		Value v;
		v.type = Type::Array;
		v.arr = std::make_shared<Array>();
		skipWs();
		if (peek() == ']') {
			p_++;
			return v;
		}
		while (true) {
			v.arr->push_back(parseValue());
			skipWs();
			char c = next();
			if (c == ']') break;
			if (c != ',') throw std::runtime_error("json: expected ',' or ']'");
		}
		return v;
	}

	static void appendUtf8(std::string& out, unsigned cp) {
		if (cp <= 0x7F) {
			out.push_back((char)cp);
		} else if (cp <= 0x7FF) {
			out.push_back((char)(0xC0 | (cp >> 6)));
			out.push_back((char)(0x80 | (cp & 0x3F)));
		} else {
			out.push_back((char)(0xE0 | (cp >> 12)));
			out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
			out.push_back((char)(0x80 | (cp & 0x3F)));
		}
	}

	Value parseString() {
		expect('"');
		Value v;
		v.type = Type::String;
		std::string& out = v.s;
		while (true) {
			char c = next();
			if (c == '"') break;
			if (c == '\\') {
				char e = next();
				switch (e) {
					case '"': out.push_back('"'); break;
					case '\\': out.push_back('\\'); break;
					case '/': out.push_back('/'); break;
					case 'n': out.push_back('\n'); break;
					case 't': out.push_back('\t'); break;
					case 'r': out.push_back('\r'); break;
					case 'b': out.push_back('\b'); break;
					case 'f': out.push_back('\f'); break;
					case 'u': {
						unsigned cp = 0;
						for (int k = 0; k < 4; k++) {
							char h = next();
							cp <<= 4;
							if (h >= '0' && h <= '9') cp |= (h - '0');
							else if (h >= 'a' && h <= 'f') cp |= (h - 'a' + 10);
							else if (h >= 'A' && h <= 'F') cp |= (h - 'A' + 10);
						}
						appendUtf8(out, cp);
						break;
					}
					default: out.push_back(e); break;
				}
			} else {
				out.push_back(c);
			}
		}
		return v;
	}

	Value parseBool() {
		Value v;
		v.type = Type::Bool;
		if (end_ - p_ >= 4 && std::string(p_, p_ + 4) == "true") {
			v.b = true;
			p_ += 4;
		} else if (end_ - p_ >= 5 && std::string(p_, p_ + 5) == "false") {
			v.b = false;
			p_ += 5;
		} else {
			throw std::runtime_error("json: bad literal");
		}
		return v;
	}

	Value parseNull() {
		if (end_ - p_ >= 4 && std::string(p_, p_ + 4) == "null") {
			p_ += 4;
			return Value();
		}
		throw std::runtime_error("json: bad literal");
	}

	Value parseNumber() {
		const char* start = p_;
		bool isFloat = false;
		if (peek() == '-' || peek() == '+') p_++;
		while (p_ < end_ && std::isdigit((unsigned char)*p_)) p_++;
		if (p_ < end_ && *p_ == '.') {
			isFloat = true;
			p_++;
			while (p_ < end_ && std::isdigit((unsigned char)*p_)) p_++;
		}
		if (p_ < end_ && (*p_ == 'e' || *p_ == 'E')) {
			isFloat = true;
			p_++;
			if (p_ < end_ && (*p_ == '+' || *p_ == '-')) p_++;
			while (p_ < end_ && std::isdigit((unsigned char)*p_)) p_++;
		}
		std::string tok(start, p_);
		if (tok.empty()) throw std::runtime_error("json: expected number");
		Value v;
		if (isFloat) {
			v.type = Type::Double;
			v.d = std::stod(tok);
		} else {
			v.type = Type::Int;
			v.i = std::stoll(tok);
		}
		return v;
	}
};

}  // namespace detail

inline Value parse(const std::string& text) {
	detail::Parser p(text.data(), text.data() + text.size());
	return p.parse();
}

}  // namespace json
