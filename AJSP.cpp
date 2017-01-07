/*
 * AJSP.cpp
 *
 *  Created on: Jan 2, 2017
 *      Author: bbielaws
 */

#include "AJSP.hpp"

#include <ctype.h>

//#define USE_ARDUINO

#ifdef USE_ARDUINO
#include <Arduino.h>
#else
#include <map>
#include <iostream>
#include <unistd.h>
#endif

using namespace AJSP;
using namespace std;

map<AJSP::Parser::Entity, std::string> AJSP::Parser::entityNames =
{
	{Parser::Entity::OBJECT, "Object"},
	{Parser::Entity::ARRAY,  "Array"},
	{Parser::Entity::VALUE,  "Value"},
	{Parser::Entity::KEY,		 "Key"},
	{Parser::Entity::STRING, "String"},
	{Parser::Entity::RAW,	   "Raw"}
};


static std::string localToString(uint32_t v)
{
	char buffer[12];
	snprintf(buffer, 12,  "%d", v);
	return std::string(buffer);
}

AJSP::Parser::Parser()
{
	stack.emplace(Entity::VALUE, State::NONE);
	localBuffer.reserve(32);
}

void AJSP::Parser::setListener(Listener* l)
{
	listener = l;
}

void AJSP::Parser::reset()
{
	localBuffer.clear();
	lastKey = rootElementName;
	offset = 0;

	stack.emplace(Entity::VALUE, State::NONE);
}

bool AJSP::Parser::skipWhitespace(char c) const
{
	auto& currentElement = stack.top();
	return isspace(c) && !(currentElement.entity == Entity::STRING && currentElement.state == State::STRING_BODY);
}


AJSP::Parser::Result AJSP::Parser::parse(char c)
{
	if (!c)
		return Result::OK;

	if (skipWhitespace(c))
	{
		offset++;
		return Result::OK;
	}

	bool consumed = false;

	while ((not consumed) and (result == Result::OK) and (not stack.empty()))
	{
		auto& currentElement = stack.top();

		//cout << "*******************************" << endl;
		//cout << "CH: " << c << endl;

		printState("BEFORE");

		switch (currentElement.entity)
		{
			case Entity::OBJECT:
				consumed = parseObject(c);
				break;

			case Entity::ARRAY:
				consumed = parseArray(c);
				break;

			case Entity::VALUE:
				consumed = parseValue(c);
				break;

			case Entity::STRING:
			case Entity::KEY:
				consumed = parseString(c);
				break;

			case Entity::RAW:
				consumed = parseRaw(c);
				break;
		}

		printState("AFTER");
	};

	if (consumed)
		offset++;

	//TODO: fix the condition, check for the error code AND if the stack is empty
	if (stack.empty() && result == Result::OK)
	{
		if (listener) listener->done();
		return Result::DONE;
	}

	return result;
}


//changes the VALUE entity from the top of the stack to the proper entity
bool AJSP::Parser::parseValue(char c)
{
	auto& currentElement = stack.top();

	if (currentElement.entity != Entity::VALUE)
	{
		reportErrorToParent(Result::INVALID_INTERNAL_STATE);
		return false;
	}

	if (c == '{')	//object - consumes the element
	{
		//NOTE: exit point
		if (listener) listener->objectStart();

		currentElement = StackElement(Entity::OBJECT, State::OBJECT_KEY_OR_END);
		return true;
	}

	if (c == '[')	//array - consumes the char
	{
		//NOTE: exit point
		if (listener) listener->arrayStart();

		//set the new lastIndex to 0, the previous one should be saved in ARRAY's state
		//to be restored when we pop
		lastKey = "0";
		currentElement = StackElement(Entity::ARRAY, (State)0);
  	return true;
	}

	if ((c == 'u') or (c == '\"') or (c == '\''))	//string
	{
		currentElement = StackElement(Entity::STRING, State::STRING_START);
		parseString(c);
		return true;
	}

	if (checkRawChar(c))
	{
		//let's see if we can handle this one with RAW entity (bool, null, numbers)
		currentElement = StackElement(Entity::RAW, State::NONE);
		localBuffer.clear();
		return parseRaw(c);
	}

	//failed to recognize character
	//reportErrorToParent(Result::INVALID_CHARACTER);
	stack.pop();
	return false;
}

bool AJSP::Parser::parseString(char c)
{
	auto& currentElement = stack.top();

	if (!(currentElement.entity == Entity::STRING or currentElement.entity == Entity::KEY))
	{
		reportErrorToParent(Result::INVALID_INTERNAL_STATE);
		return false;
	}

	switch (currentElement.state)
	{
		case State::STRING_START:
			//we should skip 'u' that is at the beginning - u for unicode
			if (c == 'u')
				return true;
			if ((c == '\"') or (c == '\''))
			{
				currentElement.state = State::STRING_BODY;	//we're in the string
				localBuffer.clear();
				return true;
			}

			reportErrorToParent(Result::IC_STRING_START_EXPECTED);
			return false;

		case State::STRING_BODY:
			if ((c == '\"') or (c == '\''))		//end of string
			{
				//NOTE: exit point
				bool isKey = currentElement.entity == Entity::KEY;

				if (isKey)
				{
					if (listener) listener->key(localBuffer);
					lastKey = localBuffer;
				}
				else
				{
					if (listener) listener->value(localBuffer);
				}

				stack.pop();
				return true;
			}

			if (c == '\\')
			{
				currentElement.state = State::STRING_ESCAPE;
				return true;
			}

			localBuffer += c;
			return true;

		case State::STRING_ESCAPE:
			switch (c)
			{
				case 'n': localBuffer += '\n'; break;
				case 'r': localBuffer += '\r'; break;
				case 't': localBuffer += '\t'; break;
				case '\\': localBuffer += '\\'; break;

				default:
					localBuffer += c;		//just put the raw value
			}

			currentElement.state = State::STRING_BODY;
			return true;

				default:;

	}

	reportErrorToParent(Result::INVALID_INTERNAL_STATE);
	return false;
}

bool AJSP::Parser::parseArray(char c)
{
	auto& currentElement = stack.top();
	if (currentElement.entity != Entity::ARRAY)
	{
		reportErrorToParent(Result::INVALID_INTERNAL_STATE);
		return false;
	}

	uint32_t currentIndex = (int)currentElement.state;

	if (currentIndex == 0)
	{
	  //try to create an element
		currentElement.state = (State)1;
		stack.emplace(Entity::VALUE, State::NONE);
		bool consumed = parseValue(c);
	  if (consumed) return true;
	}

	if (c == ',' and currentIndex != 0)
	{
		lastKey = localToString(currentIndex);
		currentElement.state = (State)(currentIndex+1);
		stack.emplace(Entity::VALUE, State::NONE);
		return true;
	}

	if (c == ']')
	{
		//NOTE: exit point
		if (listener)
			listener->arrayEnd();
		stack.pop();
		return true;
	}

	if (currentIndex == 0)
	{
		reportErrorToParent(Result::IC_ARRAY_VALUE_OR_END_EXPECTED);
	}
		else reportErrorToParent(Result::IC_ARRAY_COMMA_OR_END_EXPECTED);
	return false;
}


bool		AJSP::Parser::parseObject(char c)
{
	auto& currentElement = stack.top();
	if (currentElement.entity != Entity::OBJECT)
	{
		reportErrorToParent(Result::INVALID_INTERNAL_STATE);
		return false;
	}

	switch (currentElement.state)
	{
		case State::OBJECT_KEY_OR_END:
			if (c == '}')
			{
				//NOTE: exit point
				if (listener)
					listener->objectEnd();

				stack.pop();
				return true;
			}

			//the next thing we're expecting on this stack level
			//is a colon (after the string is done)
			currentElement.state = State::OBJECT_COLON;

			//try parsing it as a string
			{
				stack.emplace(Entity::KEY, State::STRING_START);
				bool consumed = parseString(c);

				if (!consumed and result == Result::IC_STRING_START_EXPECTED)
				{
					reportErrorToParent(Result::IC_OBJECT_KEY_OR_END_EXPECTED);
				}
				return consumed;
			}

		case State::OBJECT_COLON:
			//here we only expect K and V separator
			if (c == ':')
			{
				currentElement.state = State::OBJECT_VALUE;
				return true;
			}

			reportErrorToParent(Result::IC_OBJECT_COLON_EXPECTED);
			return false;

		case State::OBJECT_VALUE:
			stack.emplace(Entity::VALUE, State::NONE);
			if (parseValue(c))
			{
					currentElement.state = State::OBJECT_SEPARATOR_OR_END;
					return true;
			}

			reportErrorToParent(Result::IC_OBJECT_VALUE_EXPECTED);
			return false;


		case State::OBJECT_SEPARATOR_OR_END:
			if (c == '}')
			{
				//NOTE: exit point
				if (listener)
					listener->objectEnd();

				stack.pop();
				return true;
			}

			if (c == ',')
			{
				currentElement.state = State::OBJECT_COLON;
				stack.emplace(Entity::KEY, State::STRING_START);
				return true;
			}

			reportErrorToParent(Result::IC_OBJECT_SEPARATOR_OR_END_EXPECTED);
			return false;

		default:;
	}

	reportErrorToParent(Result::INVALID_CHARACTER);
	return false;
}

bool 		AJSP::Parser::checkRawChar(char c)
{
	return isalnum(c) or c == '+' or c == '-' or c == '.';
}

bool		AJSP::Parser::parseRaw(char c)
{
	auto& currentElement = stack.top();
	if (currentElement.entity != Entity::RAW)
	{
		reportErrorToParent(Result::INVALID_INTERNAL_STATE);
		return false;
	}

	/*
	 * FIXME:
	 * currently the code will accept any input that consists of these characters
	 * it could be fixed to be able to invalid sequences:
	 * -multiple dots
	 * -multiple exponents
	 * -+- signs not at the beginning or not after e/E
	 * -... and probably many more
	 *
	 * And it doesn't support unicode escapes...
	 */

	if (checkRawChar(c))
	{
		localBuffer += c;
		return true;
	}

	//if we already had something in the buffer then it's the end of the token
	if (localBuffer.length() && listener)
	{
		//NOTE: exit point
		listener->value(localBuffer);
		localBuffer.clear();
	}

	stack.pop();
	return false;
}

const char* AJSP::Parser::getResultDescription(Result r)
{
	 switch (r)
	 {
		 case Result::OK:
			 return "OK";
		 case Result::DONE:
			 return "Done";
		 case Result::INVALID_CHARACTER:
			 return "Invalid character";
		 case Result::IC_STRING_START_EXPECTED:
			 return "String start expected";
		 case Result::IC_ARRAY_COMMA_OR_END_EXPECTED:
			 return "Array separator or end brace expected";
			case Result::IC_ARRAY_VALUE_OR_END_EXPECTED:
			 return "Value or end brace expected";
		 case Result::IC_OBJECT_COLON_EXPECTED:
			 return "Colon expected";
		 case Result::IC_OBJECT_VALUE_EXPECTED:
		 	return "Value expected";
		 case Result::IC_OBJECT_KEY_OR_END_EXPECTED:
			 return "Key or end brace expected";
		 case Result::IC_OBJECT_SEPARATOR_OR_END_EXPECTED:
			 return "Comma or end brace expected";
		 case Result::INVALID_INTERNAL_STATE:
			 return "Invalid internal state";
	 }

	 return "Unknown";
}

void 	  AJSP::Parser::printState(const std::string& msg) const
{
	return;
	cout << "=================  " << msg << "  ==============" << endl;
	cout << "StackSize:   " << stack.size() << endl;
	cout << "Top element: " << entityNames[stack.top().entity] << endl;
	cout << "Offset:      " << offset << endl;
	cout << "Result:      " << getResultDescription(result) << endl;
	cout << "State:       " << int(result) << endl;
}

void AJSP::Parser::printStack() const
{
	auto stackCpy = stack;
	auto mapCpy = entityNames;

	while (!stackCpy.empty())
	{
		cout << mapCpy[stackCpy.top().entity] << endl;
	}
}

void AJSP::Parser::reportErrorToParent(Result r)
{
	result = r;
//	if (stack.size() == 0)
//		return;
//
//	stack.pop();
//	stack.top().state = State::INVALID;
}
