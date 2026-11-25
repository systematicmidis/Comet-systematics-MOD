#include "MenuItem.h"

class MenuBuilder
{
public:
	MenuBuilder() = default;
	~MenuBuilder()
	{
		Clear();
	}

	void Clear()
	{
		for (SubMenu& menu : menus)
		{
			for (MenuItem* item : menu.GetItems())
			{
				delete item;
			}
		}
		menus.clear();
	}

	SubMenu& CreateMenu(std::string label)
	{
		menus.emplace_back(std::move(label));
		return menus.back();
	}
	void Draw()
	{
		if (ImGui::BeginMainMenuBar())
		{
			for (SubMenu& menu : menus)
			{
				menu.Draw();
			}
			ImGui::EndMainMenuBar();
		}
	}

	MenuBuilder(const MenuBuilder&) = delete;
	MenuBuilder& operator=(const MenuBuilder&) = delete;
private:
	std::vector<SubMenu> menus = { };
};
