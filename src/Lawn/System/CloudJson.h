/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable (local cloud save patch).
 */

#ifndef __CLOUDJSON_H__
#define __CLOUDJSON_H__

#include <cstdint>
#include <cstdlib>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Just enough JSON for the OAuth and cloud drive answers: parse a document, look up members, quote a string.
namespace CloudSync::Json
{

struct Value
{
	enum class Type
	{
		NUL,
		BOOL,
		NUMBER,
		STRING,
		ARRAY,
		OBJECT,
	};

	Type										mType = Type::NUL;
	bool										mBool = false;
	double										mNumber = 0;
	std::string									mString;	// for numbers too: the text as written
	std::vector<Value>							mArray;
	std::vector<std::pair<std::string, Value>>	mObject;

	const Value* Find(std::string_view theKey) const
	{
		for (const auto& [aKey, aValue] : mObject)
		{
			if (aKey == theKey)
				return &aValue;
		}
		return nullptr;
	}

	// A string or number member as text, "" when missing.
	std::string Str(std::string_view theKey) const
	{
		const Value* aValue = Find(theKey);
		return aValue && (aValue->mType == Type::STRING || aValue->mType == Type::NUMBER) ? aValue->mString : std::string();
	}

	// A number member, or a string holding one (Google sends 64-bit numbers as strings).
	int64_t Int(std::string_view theKey, int64_t theDefault) const
	{
		const Value* aValue = Find(theKey);
		if (aValue == nullptr || (aValue->mType != Type::NUMBER && aValue->mType != Type::STRING) || aValue->mString.empty())
			return theDefault;
		char* anEnd = nullptr;
		long long aNumber = std::strtoll(aValue->mString.c_str(), &anEnd, 10);
		return anEnd == aValue->mString.c_str() ? theDefault : aNumber;
	}
};

namespace Detail
{

class Parser
{
	std::string_view	mText;
	size_t				mPos = 0;
	int					mDepth = 0;

	void SkipSpace()
	{
		while (mPos < mText.size() && (mText[mPos] == ' ' || mText[mPos] == '\t' || mText[mPos] == '\r' || mText[mPos] == '\n'))
			mPos++;
	}

	bool Literal(std::string_view theWord)
	{
		if (mText.substr(mPos, theWord.size()) != theWord)
			return false;
		mPos += theWord.size();
		return true;
	}

	static void AppendUtf8(std::string& theOut, uint32_t theCode)
	{
		if (theCode < 0x80)
			theOut += static_cast<char>(theCode);
		else if (theCode < 0x800)
		{
			theOut += static_cast<char>(0xC0 | (theCode >> 6));
			theOut += static_cast<char>(0x80 | (theCode & 0x3F));
		}
		else if (theCode < 0x10000)
		{
			theOut += static_cast<char>(0xE0 | (theCode >> 12));
			theOut += static_cast<char>(0x80 | ((theCode >> 6) & 0x3F));
			theOut += static_cast<char>(0x80 | (theCode & 0x3F));
		}
		else
		{
			theOut += static_cast<char>(0xF0 | (theCode >> 18));
			theOut += static_cast<char>(0x80 | ((theCode >> 12) & 0x3F));
			theOut += static_cast<char>(0x80 | ((theCode >> 6) & 0x3F));
			theOut += static_cast<char>(0x80 | (theCode & 0x3F));
		}
	}

	bool Hex4(uint32_t& theCode)
	{
		if (mPos + 4 > mText.size())
			return false;
		theCode = 0;
		for (int i = 0; i < 4; i++)
		{
			char c = mText[mPos++];
			theCode <<= 4;
			if (c >= '0' && c <= '9')
				theCode |= c - '0';
			else if (c >= 'a' && c <= 'f')
				theCode |= c - 'a' + 10;
			else if (c >= 'A' && c <= 'F')
				theCode |= c - 'A' + 10;
			else
				return false;
		}
		return true;
	}

	bool String(std::string& theOut)
	{
		if (mPos >= mText.size() || mText[mPos] != '"')
			return false;
		mPos++;
		while (mPos < mText.size())
		{
			char c = mText[mPos++];
			if (c == '"')
				return true;
			if (static_cast<unsigned char>(c) < 0x20)
				return false;
			if (c != '\\')
			{
				theOut += c;
				continue;
			}
			if (mPos >= mText.size())
				return false;
			char anEscape = mText[mPos++];
			switch (anEscape)
			{
			case '"':	theOut += '"'; break;
			case '\\':	theOut += '\\'; break;
			case '/':	theOut += '/'; break;
			case 'b':	theOut += '\b'; break;
			case 'f':	theOut += '\f'; break;
			case 'n':	theOut += '\n'; break;
			case 'r':	theOut += '\r'; break;
			case 't':	theOut += '\t'; break;
			case 'u':
			{
				uint32_t aCode;
				if (!Hex4(aCode))
					return false;
				if (aCode >= 0xD800 && aCode < 0xDC00 && mText.substr(mPos, 2) == "\\u")
				{
					mPos += 2;
					uint32_t aLow;
					if (!Hex4(aLow) || aLow < 0xDC00 || aLow >= 0xE000)
						return false;
					aCode = 0x10000 + ((aCode - 0xD800) << 10) + (aLow - 0xDC00);
				}
				AppendUtf8(theOut, aCode);
				break;
			}
			default:
				return false;
			}
		}
		return false;
	}

public:
	explicit Parser(std::string_view theText) : mText(theText) {}

	bool ParseValue(Value& theValue)
	{
		if (++mDepth > 64)
			return false;
		SkipSpace();
		if (mPos >= mText.size())
			return false;

		bool anOk = true;
		char c = mText[mPos];
		if (c == '{')
		{
			theValue.mType = Value::Type::OBJECT;
			mPos++;
			SkipSpace();
			if (mPos < mText.size() && mText[mPos] == '}')
				mPos++;
			else
			{
				while (anOk)
				{
					SkipSpace();
					std::string aKey;
					Value aMember;
					anOk = String(aKey);
					SkipSpace();
					anOk = anOk && mPos < mText.size() && mText[mPos++] == ':' && ParseValue(aMember);
					if (!anOk)
						break;
					theValue.mObject.emplace_back(std::move(aKey), std::move(aMember));
					SkipSpace();
					if (mPos < mText.size() && mText[mPos] == ',')
						mPos++;
					else
					{
						anOk = mPos < mText.size() && mText[mPos++] == '}';
						break;
					}
				}
			}
		}
		else if (c == '[')
		{
			theValue.mType = Value::Type::ARRAY;
			mPos++;
			SkipSpace();
			if (mPos < mText.size() && mText[mPos] == ']')
				mPos++;
			else
			{
				while (anOk)
				{
					Value anItem;
					anOk = ParseValue(anItem);
					if (!anOk)
						break;
					theValue.mArray.push_back(std::move(anItem));
					SkipSpace();
					if (mPos < mText.size() && mText[mPos] == ',')
						mPos++;
					else
					{
						anOk = mPos < mText.size() && mText[mPos++] == ']';
						break;
					}
				}
			}
		}
		else if (c == '"')
		{
			theValue.mType = Value::Type::STRING;
			anOk = String(theValue.mString);
		}
		else if (Literal("true") || Literal("false"))
		{
			theValue.mType = Value::Type::BOOL;
			theValue.mBool = mText[mPos - 1] == 'e' && mText[mPos - 2] == 'u';
		}
		else if (Literal("null"))
			theValue.mType = Value::Type::NUL;
		else
		{
			size_t aStart = mPos;
			while (mPos < mText.size() && std::string_view("+-0123456789.eE").find(mText[mPos]) != std::string_view::npos)
				mPos++;
			theValue.mType = Value::Type::NUMBER;
			theValue.mString = std::string(mText.substr(aStart, mPos - aStart));
			char* anEnd = nullptr;
			theValue.mNumber = std::strtod(theValue.mString.c_str(), &anEnd);
			anOk = !theValue.mString.empty() && anEnd == theValue.mString.c_str() + theValue.mString.size();
		}
		mDepth--;
		return anOk;
	}

	bool AtEnd()
	{
		SkipSpace();
		return mPos == mText.size();
	}
};

}

inline bool Parse(std::string_view theText, Value& theValue)
{
	theValue = Value();
	Detail::Parser aParser(theText);
	return aParser.ParseValue(theValue) && aParser.AtEnd();
}

inline std::string Quote(std::string_view theText)
{
	std::string anOut = "\"";
	for (unsigned char c : theText)
	{
		if (c == '"' || c == '\\')
		{
			anOut += '\\';
			anOut += static_cast<char>(c);
		}
		else if (c < 0x20)
			anOut += std::format("\\u{:04x}", static_cast<unsigned>(c));
		else
			anOut += static_cast<char>(c);
	}
	return anOut + '"';
}

}

#endif
