#pragma once

#ifdef __APPLE__

#include <string>

#import <AppKit/AppKit.h>

NSString* MacOSToNSString(const std::string& value);
NSString* MacOSToNSString(const std::wstring& value);
NSString* MacOSToNSString(const wchar_t* value);
std::string MacOSToStdString(NSString* value);

NSTextField* MacOSMakeLabel(NSString* text);
NSTextField* MacOSMakeWrappingLabel(NSString* text, CGFloat fontSize, NSColor* color);
NSTextField* MacOSMakeTextField(CGFloat minWidth);
NSTextField* MacOSMakeFixedWidthTextField(CGFloat width);
NSSecureTextField* MacOSMakeSecureTextField(CGFloat minWidth);
NSBox* MacOSMakeGroupBox();
NSImage* MacOSMakeSymbolImage(NSString* symbolName, NSString* accessibilityDescription, CGFloat pointSize, NSColor* tintColor);
NSImageView* MacOSMakeSymbolImageView(NSString* symbolName, NSString* accessibilityDescription, NSColor* tintColor);
NSButton* MacOSMakeIconButton(NSString* symbolName, NSString* accessibilityDescription, id target, SEL action);

void MacOSSetFieldText(NSTextField* field, NSString* value);
void MacOSSetFieldText(NSTextField* field, const std::string& value);
void MacOSSetFieldText(NSTextField* field, int value);

#endif
