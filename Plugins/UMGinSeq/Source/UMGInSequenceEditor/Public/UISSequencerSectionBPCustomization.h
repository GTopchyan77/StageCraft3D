//  Copyright WASD Studio 2025 All Rights Reserved.

#pragma once

#include "IDetailCustomization.h"

class IDetailsView;
class IDetailLayoutBuilder;

class FUISSequencerSectionBPCustomization : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance();

	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;

private:
	TSharedPtr<IDetailsView> WidgetDetailsView;
};
