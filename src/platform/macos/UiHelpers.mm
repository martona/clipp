#include "UiHelpers.h"

#ifdef __APPLE__

#include "platform.h"

NSString* MacOSToNSString(const std::string& value) {
    NSString* text = [NSString stringWithUTF8String:value.c_str()];
    return text != nil ? text : @"";
}

NSString* MacOSToNSString(const std::wstring& value) {
    const size_t utf8Len = utf16_to_utf8(value.c_str(), value.size(), nullptr, 0);
    if (utf8Len == 0) {
        return @"";
    }

    std::string utf8;
    utf8.resize(utf8Len);
    utf16_to_utf8(value.c_str(), value.size(), utf8.data(), utf8.size());
    return MacOSToNSString(utf8);
}

NSString* MacOSToNSString(const wchar_t* value) {
    return value != nullptr ? MacOSToNSString(std::wstring(value)) : nil;
}

std::string MacOSToStdString(NSString* value) {
    if (value == nil) {
        return {};
    }

    const char* utf8 = value.UTF8String;
    return utf8 != nullptr ? std::string(utf8) : std::string();
}

NSTextField* MacOSMakeLabel(NSString* text) {
    NSTextField* label = [NSTextField labelWithString:text];
    label.translatesAutoresizingMaskIntoConstraints = NO;
    label.font = [NSFont systemFontOfSize:13];
    label.textColor = [NSColor labelColor];
    return label;
}

NSTextField* MacOSMakeWrappingLabel(NSString* text, CGFloat fontSize, NSColor* color) {
    NSTextField* label = [NSTextField wrappingLabelWithString:text];
    label.translatesAutoresizingMaskIntoConstraints = NO;
    label.font = [NSFont systemFontOfSize:fontSize];
    label.textColor = color;
    return label;
}

NSTextField* MacOSMakeTextField(CGFloat minWidth) {
    NSTextField* field = [NSTextField textFieldWithString:@""];
    field.translatesAutoresizingMaskIntoConstraints = NO;
    field.font = [NSFont systemFontOfSize:13];
    field.alignment = NSTextAlignmentLeft;
    [field.widthAnchor constraintGreaterThanOrEqualToConstant:minWidth].active = YES;
    return field;
}

NSTextField* MacOSMakeFixedWidthTextField(CGFloat width) {
    NSTextField* field = [NSTextField textFieldWithString:@""];
    field.translatesAutoresizingMaskIntoConstraints = NO;
    field.font = [NSFont systemFontOfSize:13];
    field.alignment = NSTextAlignmentLeft;
    [field.widthAnchor constraintEqualToConstant:width].active = YES;
    return field;
}

NSSecureTextField* MacOSMakeSecureTextField(CGFloat minWidth) {
    NSSecureTextField* field = [[NSSecureTextField alloc] initWithFrame:NSZeroRect];
    field.translatesAutoresizingMaskIntoConstraints = NO;
    field.font = [NSFont systemFontOfSize:13];
    field.alignment = NSTextAlignmentLeft;
    field.contentType = NSTextContentTypePassword;
    [field.widthAnchor constraintGreaterThanOrEqualToConstant:minWidth].active = YES;
    [field.heightAnchor constraintEqualToConstant:22.0].active = YES;
    return field;
}

NSBox* MacOSMakeGroupBox() {
    NSBox* box = [[NSBox alloc] initWithFrame:NSZeroRect];
    box.translatesAutoresizingMaskIntoConstraints = NO;
    box.boxType = NSBoxCustom;
    box.titlePosition = NSNoTitle;
    box.borderType = NSNoBorder;
    box.cornerRadius = 10.0;
    box.fillColor = [NSColor alternatingContentBackgroundColors][1];
    return box;
}

NSImage* MacOSMakeSymbolImage(NSString* symbolName, NSString* accessibilityDescription, CGFloat pointSize, NSColor* tintColor) {
    NSImage* image = [NSImage imageWithSystemSymbolName:symbolName accessibilityDescription:accessibilityDescription];
    if (image == nil) {
        return nil;
    }

    NSImageSymbolConfiguration* config = [NSImageSymbolConfiguration configurationWithPointSize:pointSize weight:NSFontWeightMedium];
    image = [image imageWithSymbolConfiguration:config];
    [image setTemplate:YES];
    (void)tintColor;
    return image;
}

NSImageView* MacOSMakeSymbolImageView(NSString* symbolName, NSString* accessibilityDescription, NSColor* tintColor) {
    NSImageView* imageView = [[NSImageView alloc] initWithFrame:NSZeroRect];
    imageView.translatesAutoresizingMaskIntoConstraints = NO;
    imageView.image = MacOSMakeSymbolImage(symbolName, accessibilityDescription, 16.0, tintColor);
    imageView.contentTintColor = tintColor;
    imageView.imageScaling = NSImageScaleProportionallyDown;
    [imageView.widthAnchor constraintEqualToConstant:22.0].active = YES;
    [imageView.heightAnchor constraintEqualToConstant:22.0].active = YES;
    return imageView;
}

NSButton* MacOSMakeIconButton(NSString* symbolName, NSString* accessibilityDescription, id target, SEL action) {
    NSButton* button = [NSButton buttonWithImage:MacOSMakeSymbolImage(symbolName, accessibilityDescription, 13.0, [NSColor secondaryLabelColor])
                                          target:target
                                          action:action];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    button.bezelStyle = NSBezelStyleRegularSquare;
    button.bordered = NO;
    button.imagePosition = NSImageOnly;
    button.contentTintColor = [NSColor secondaryLabelColor];
    button.toolTip = accessibilityDescription;
    [button.widthAnchor constraintEqualToConstant:28.0].active = YES;
    [button.heightAnchor constraintEqualToConstant:28.0].active = YES;
    return button;
}

void MacOSSetFieldText(NSTextField* field, NSString* value) {
    if (field == nil) {
        return;
    }

    field.stringValue = value != nil ? value : @"";
}

void MacOSSetFieldText(NSTextField* field, const std::string& value) {
    MacOSSetFieldText(field, MacOSToNSString(value));
}

void MacOSSetFieldText(NSTextField* field, int value) {
    if (field != nil) {
        field.stringValue = [NSString stringWithFormat:@"%d", value];
    }
}

#endif
