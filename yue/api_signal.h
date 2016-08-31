// Copyright 2016 Cheng Zhao. All rights reserved.
// Use of this source code is governed by the license that can be found in the
// LICENSE file.

#ifndef YUE_API_SIGNAL_H_
#define YUE_API_SIGNAL_H_

#include <string>

#include "lua/lua.h"
#include "nativeui/signal.h"

namespace yue {

// Get type from member pointer.
template<typename T> struct ExtractMemberPointer;
template<typename TType, typename TMember>
struct ExtractMemberPointer<TMember(TType::*)> {
  using Type = TType;
  using Member = TMember;
};

// The wrapper for nu::Signal.
class SignalBase {
 public:
  virtual int Connect(lua::CallContext* context) = 0;
  virtual void Disconnect(lua::CallContext* context, int id) = 0;
  virtual void DisconnectAll(lua::CallContext* context) = 0;
};

template<typename T>
class Signal : SignalBase {
 public:
  using Type = typename ExtractMemberPointer<T>::Type;
  using Slot = typename ExtractMemberPointer<T>::Member::Slot;

  // The singal doesn't store the object or the member directly, instead it only
  // keeps a weak reference to the object and stores the pointer to member, so
  // it can still work when user copies the signal and uses it after the object
  // gets deleted.
  Signal(lua::State* state, int index, T member)
      : object_ref_(lua::CreateWeakReference(state, index)),
        member_(member) {}

  int Connect(lua::CallContext* context) override {
    Slot slot;
    if (!lua::Pop(context->state, &slot)) {
      context->has_error = true;
      lua::Push(context->state, "first arg must be function");
      return -1;
    }

    Type* object;
    if (!GetObject(context, &object))
      return -1;

    return (object->*member_).Connect(slot);
  }

  void Disconnect(lua::CallContext* context, int id) override {
    Type* object;
    if (!GetObject(context, &object))
      return;

    (object->*member_).Disconnect(id);
  }

  void DisconnectAll(lua::CallContext* context) override {
    Type* object;
    if (!GetObject(context, &object))
      return;

    (object->*member_).DisconnectAll();
  }

 private:
  bool GetObject(lua::CallContext* context, Type** object) {
    lua::PushWeakReference(context->state, object_ref_);
    if (!lua::Pop(context->state, object)) {
      context->has_error = true;
      lua::Push(context->state, "owner of signal is gone");
      return false;
    }
    return true;
  }

  int object_ref_;
  T member_;
};

// Push a weak table which records the object's members.
void PushObjectMembersTable(lua::State* state, int index);

// Set metatable for signal.
void SetSignalMetaTable(lua::State* state, int index);

// Helper to compare name and key and create the signal wrapepr.
inline bool PushSignal(lua::State* state, const std::string& name) {
  return false;
}

template<typename T, typename... Rest>
inline bool PushSignal(lua::State* state, const std::string& name,
                       const char* key, T member, Rest... rest) {
  if (name == key) {
    // Create the wrapper for signal class.
    void* memory = lua_newuserdata(state, sizeof(Signal<T>));
    new(memory) Signal<T>(state, 1, member);
    static_assert(std::is_trivially_destructible<Signal<T>>::value,
                  "we are not providing __gc so Signal<T> must be trivial");
    return true;
  }
  return PushSignal(state, name, rest...);
}

// Define the __index handler for signals.
template<typename T, typename... Rest>
inline bool SignalIndex(lua::State* state, const std::string& name,
                        const char* key, T member, Rest... rest) {
  int top = lua::GetTop(state);
  // Check if the member has already been converted.
  PushObjectMembersTable(state, 1);
  lua::RawGet(state, -1, name);
  if (lua::GetType(state, -1) != lua::LuaType::UserData) {
    if (!PushSignal(state, name, key, member, rest...)) {
      lua::SetTop(state, top);
      return false;
    }
    SetSignalMetaTable(state, -1);
    lua::RawSet(state, top + 1, name, lua::ValueOnStack(state, -1));
  }
  // Pop the table and keep the signal.
  lua::Insert(state, top + 1);
  lua::SetTop(state, top + 1);
  DCHECK_EQ(lua::GetType(state, -1), lua::LuaType::UserData);
  return true;
}

// Defines how a member is assigned.
template<typename T, typename Enable = void>
struct MemberAssignment {};

// Assignment for signals.
template<typename T>
struct MemberAssignment<T, typename std::enable_if<std::is_class<
    typename ExtractMemberPointer<T>::Member::Slot>::value>::type> {
  static void Do(lua::State* state,
                 typename ExtractMemberPointer<T>::Type* object, T member) {
    (object->*member).DisconnectAll();
    typename ExtractMemberPointer<T>::Member::Slot slot;
    if (lua::To(state, 3, &slot))
      (object->*member).Connect(slot);
  }
};

// Assignment for delegates.
template<typename T>
struct MemberAssignment<T, typename std::enable_if<
    std::is_member_function_pointer<
        decltype(&ExtractMemberPointer<T>::Member::Reset)>::value>::type> {
  static void Do(lua::State* state,
                 typename ExtractMemberPointer<T>::Type* object, T member) {
    typename ExtractMemberPointer<T>::Member slot;
    if (lua::To(state, 3, &slot))
      (object->*member) = slot;
    else
      (object->*member).Reset();
  }
};

// Helper to compare name and key and set the members.
template<typename T>
inline bool MemberNewIndexHelper(
    lua::State* state, T object, const std::string& name) {
  return false;
}

template<typename T, typename Member, typename... Rest>
inline bool MemberNewIndexHelper(
    lua::State* state, T object, const std::string& name,
    const char* key, Member member, Rest... rest) {
  if (name == key) {
    MemberAssignment<Member>::Do(state, object, member);
    return true;
  } else {
    return MemberNewIndexHelper(state, object, name, rest...);
  }
}

// Define the __newindex handler for members.
template<typename Member, typename... Rest>
inline bool MemberNewIndex(
    lua::State* state, const std::string& name, const char* key, Member member,
    Rest... rest) {
  typename ExtractMemberPointer<Member>::Type* object;
  if (!lua::To(state, 1, &object))
    return false;
  return MemberNewIndexHelper(state, object, name, key, member, rest...);
}

}  // namespace yue

namespace lua {

template<>
struct Type<yue::SignalBase*> {
  static constexpr const char* name = "yue.Signal";
  static bool To(State* state, int index, yue::SignalBase** out);
};

}  // namespace lua

#endif  // YUE_API_SIGNAL_H_
