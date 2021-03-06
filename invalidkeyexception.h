#ifndef INVALIDKEYEXCEPTION_H
#define INVALIDKEYEXCEPTION_H

#include "whisperexception.h"

class InvalidKeyException : public WhisperException
{
public:
    InvalidKeyException(const QString &error): WhisperException("InvalidKeyException", error) {}
};

#endif // INVALIDKEYEXCEPTION_H
