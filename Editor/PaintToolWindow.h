#pragma once
class EditorComponent;

class PaintToolWindow : public wi::gui::Window
{
	float rot = 0;
	float stroke_dist = 0;
	size_t history_textureIndex = 0;
	struct TextureSlot
	{
		wi::graphics::Texture texture;
		int srgb_subresource = -1;
	};
	wi::vector<TextureSlot> history_textures;
	TextureSlot GetEditTextureSlot(const wi::scene::MaterialComponent& material, int* uvset = nullptr);
	void ReplaceEditTextureSlot(wi::scene::MaterialComponent& material, const TextureSlot& textureslot);

	wi::unordered_map<wi::ecs::Entity, wi::Archive> historyStartDatas;

	struct SculptingIndex
	{
		size_t ind;
		float affection;
	};
	wi::vector<SculptingIndex> sculpting_indices;
	XMFLOAT3 sculpting_normal = XMFLOAT3(0, 0, 0);

	wi::Resource brushTex;
	wi::Resource revealTex;

	struct Stroke
	{
		XMFLOAT2 position;
		float pressure;
	};
	std::deque<Stroke> strokes;

public:
	void Create(EditorComponent* editor);

	EditorComponent* editor = nullptr;

	wi::gui::ComboBox modeComboBox;
	wi::gui::Label infoLabel;
	wi::gui::Slider radiusSlider;
	wi::gui::Slider amountSlider;
	wi::gui::Slider smoothnessSlider;
	wi::gui::Slider spacingSlider;
	wi::gui::Slider rotationSlider;
	wi::gui::Slider stabilizerSlider;
	wi::gui::CheckBox backfaceCheckBox;
	wi::gui::CheckBox wireCheckBox;
	wi::gui::CheckBox pressureCheckBox;
	wi::gui::CheckBox alphaCheckBox;
	wi::gui::CheckBox terrainCheckBox;
	wi::gui::ColorPicker colorPicker;
	wi::gui::ComboBox textureSlotComboBox;
	wi::gui::ComboBox brushShapeComboBox;
	wi::gui::Button saveTextureButton;
	wi::gui::Button brushTextureButton;
	wi::gui::Button revealTextureButton;
	wi::gui::ComboBox axisCombo;

	void UpdateData(float dt);
	void DrawBrush(const wi::Canvas& canvas, wi::graphics::CommandList cmd) const;

	XMFLOAT2 pos = XMFLOAT2(0, 0);
	wi::scene::PickResult brushIntersect;

	enum MODE
	{
		MODE_DISABLED,
		MODE_TEXTURE,
		MODE_VERTEXCOLOR,
		MODE_SCULPTING_ADD,
		MODE_SCULPTING_SUBTRACT,
		MODE_SOFTBODY_PINNING,
		MODE_SOFTBODY_PHYSICS,
		MODE_HAIRPARTICLE_ADD_TRIANGLE,
		MODE_HAIRPARTICLE_REMOVE_TRIANGLE,
		MODE_HAIRPARTICLE_LENGTH,
		MODE_TERRAIN_MATERIAL,
		MODE_WIND,
	};
	MODE GetMode() const;

	enum class AxisLock
	{
		Disabled,
		X,
		Y,
		Z
	};

	wi::vector<wi::gui::Button> terrain_material_buttons;
	size_t terrain_material_layer = 0;

	float texture_paint_radius = 50;
	float vertex_paint_radius = 0.1f;
	float terrain_paint_radius = 5;

	wi::Archive* currentHistory = nullptr;
	void WriteHistoryData(wi::ecs::Entity entity, wi::Archive& archive, wi::graphics::CommandList cmd = wi::graphics::CommandList());
	void RecordHistory(wi::ecs::Entity entity, wi::graphics::CommandList cmd = wi::graphics::CommandList());
	void ConsumeHistoryOperation(wi::Archive& archive, bool undo);

	void ResizeLayout() override;

	void RecreateTerrainMaterialButtons();
};
