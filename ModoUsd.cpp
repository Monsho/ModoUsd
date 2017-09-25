// ModoUsd.cpp : コンソール アプリケーションのエントリ ポイントを定義します。
//

#include <lxu_scene.hpp>
#include <lxu_math.hpp>
#include <lxidef.h>
#include <lx_action.hpp>
#include <lx_trisurf.hpp>
#include <lx_value.hpp>

#include <lxu_prefvalue.hpp>
#include <lxu_queries.hpp>
#include <lxu_log.hpp>
#include <lxlog.h>
#include <lxu_select.hpp>

#include "pxr/pxr.h"
#include "pxr/base/gf/gamma.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usdGeom/mesh.h"
#include "pxr/usd/usdGeom/sphere.h"
#include "pxr/usd/usdGeom/xform.h"
#include "pxr/usd/usdGeom/xformCommonAPI.h"
#include "pxr/usd/usdGeom/scope.h"
#include "pxr/usd/usdShade/material.h"
#include "pxr/usd/usdShade/shader.h"
#include "pxr/usd/usdRi/risBxdf.h"
#include "pxr/usd/usdRi/materialAPI.h"
#include "pxr/usd/usdUtils/pipeline.h"


namespace pxr
{
	TF_DEFINE_PRIVATE_TOKENS(
		_tokens,
		(displayColor)
		(displayOpacity)
		(diffuseColor)
		(transmissionColor)
		(transparency)
	);
}


namespace ModoUsd
{
	// 単位
	enum UnitsType
	{
		UNITS_NONE = -1,
		UNITS_M,
		UNITS_CM,
		UNITS_MM,
		UNITS_INCH,

		UNITS_COUNT
	};

	// 設定の値を取得する
	static int GetUserInt(const char *prefKey, int defaultValue = 0)
	{
		int	value = defaultValue;
		CLxReadUserValue ruvUser;
		if (ruvUser.Query(prefKey))
		{
			value = ruvUser.GetInt();
		}

		return value;
	}

	// パスとして有効な名前を取得する
	static std::string GetValidPathName(const std::string& name)
	{
		std::string ret = name;
		for (auto it = ret.begin(); it != ret.end(); ++it)
		{
			if (*it == ' ' || *it == ':' || *it == ';')
			{
				*it = '_';
			}
		}
		auto pos = ret.find('(');
		while (pos != std::string::npos)
		{
			ret.erase(pos, 1);
			pos = ret.find('(');
		}
		pos = ret.find(')');
		while (pos != std::string::npos)
		{
			ret.erase(pos, 1);
			pos = ret.find(')');
		}
		return ret;
	}

	// エクスポータ
	class CUsdSaver
		: public CLxSceneSaver
		, public CLxFileFormat
	{
	public:
		~CUsdSaver()
		{
			ff_Cleanup();
		}

		// TagInfo
		static LXtTagInfoDesc	 descInfo[];

		// CLxFileFormat
		bool	ff_Open(const char* f) LXx_OVERRIDE
		{
			ff_Cleanup();

			usd_file_name_ = f;
			file_name = usd_file_name_.c_str();

			usd_stage_ = pxr::UsdStage::CreateNew(f);
			if (usd_stage_ == nullptr)
			{
				return false;
			}

			return true;
		}
		void	ff_Enable(bool b) LXx_OVERRIDE
		{
			can_output_ = b;
		}
		bool	ff_HasError() LXx_OVERRIDE
		{
			// TODO: エラー対応
			return usd_file_name_.empty();
		}
		void	ff_Cleanup() LXx_OVERRIDE
		{
			if (usd_stage_ != nullptr)
			{
				usd_stage_->GetRootLayer()->Save();
				usd_stage_.Reset();
			}

			file_name = nullptr;
			usd_file_name_.clear();
			can_output_ = false;
		}

		// CLxSceneSaver
		CLxFileFormat*	 ss_Format() LXx_OVERRIDE
		{
			return this;
		}
		LxResult	 ss_Save() LXx_OVERRIDE
		{
			if (!can_output_)
			{
				// TODO: データ検証用の処理.
				//       実際にはエクスポートしない
			}
			else
			{
				// 単位スケールを求める
				int value = GetUserInt("sceneio.obj.import.units");
				if (value < 0 || value > UNITS_COUNT)
				{
					value = UNITS_NONE;
				}
				switch (value)
				{
				case UNITS_MM:
					unit_scale_ = 1000.0;
					break;

				case UNITS_CM:
					unit_scale_ = 100.0;
					break;

				case UNITS_INCH:
					unit_scale_ = 39.3701;
					break;

				case UNITS_M:
				default:
					unit_scale_ = 1.0;
					break;
				}

				// ルートのXFormをエクスポート
				auto xform = pxr::UsdGeomXform::Define(usd_stage_, pxr::SdfPath("/root"));

				// メッシュのイテレート
				StartScan();
				while (NextMesh())
				{
					WriteMesh();
				}

				// マテリアルのエクスポート
				WriteMaterial();
			}
			return LXe_OK;
		}
		void	ss_Point()
		{
			float			 opos[3];
			double			 vec[3];

			CLxUser_SceneService svc;
			if (GetItemType() != svc.ItemType(LXsITYPE_MESH))
			{
				return;
			}

			mesh_point_id_to_index_[PntID()] = mesh_point_count_++;

			PntPosition(opos);

			lx::MatrixMultiply(vec, current_mtx_, opos);
			vec[0] += current_pos_[0];
			vec[1] += current_pos_[1];
			vec[2] += current_pos_[2];

			vec[0] *= unit_scale_;
			vec[1] *= unit_scale_;
			vec[2] *= unit_scale_;

			usd_points_.push_back(pxr::GfVec3f(static_cast<float>(vec[0]), static_cast<float>(vec[1]), static_cast<float>(vec[2])));
		}
		void	ss_Polygon()
		{
			// このポリゴンがアサインされているマテリアルを調べる
			std::string mat_name = PolyTag(LXi_PTAG_MATR);
			auto assigned_mat = modo_assign_material_and_face_index_.find(mat_name);
			if (assigned_mat != modo_assign_material_and_face_index_.end())
			{
				// このマテリアルがアサインされているポリゴンがすでに存在する
				auto path_face = assigned_mat->second.find(usd_curr_mesh_path_);
				if (path_face != assigned_mat->second.end())
				{
					path_face->second.push_back((int)usd_index_count_by_face_.size());
				}
				else
				{
					pxr::VtArray<int> poly_indices;
					poly_indices.push_back((int)usd_index_count_by_face_.size());
					assigned_mat->second.insert(std::pair<pxr::SdfPath, pxr::VtArray<int>>(usd_curr_mesh_path_, poly_indices));
				}
			}
			else
			{
				pxr::VtArray<int> poly_indices;
				poly_indices.push_back((int)usd_index_count_by_face_.size());
				PathFaceArray pf_array;
				pf_array.insert(std::pair<pxr::SdfPath, pxr::VtArray<int>>(usd_curr_mesh_path_, poly_indices));
				modo_assign_material_and_face_index_.insert(std::pair<std::string, PathFaceArray>(mat_name, pf_array));
			}

			auto n = PolyNumVerts();
			usd_index_count_by_face_.push_back(n);

			for (unsigned int i = 0; i < n; i++)
			{
				// Vertex Index

				// CCWにポリゴンの順番を変更
				unsigned vInd = (current_mtx_determinant_ > 0) ? i : (n - 1 - i);

				auto pnt = PolyVertex(vInd);
				auto vrt = mesh_point_id_to_index_[pnt];

				usd_face_indices_.push_back(vrt);

				// Normal
				double local_normal[3], world_normal[3];
				if (!PolyNormal(local_normal, pnt))
				{
					local_normal[0] = 0.0;
					local_normal[1] = 1.0;
					local_normal[2] = 0.0;
				}

				lx::MatrixMultiply(world_normal, current_mtx_, local_normal);
				auto N = pxr::GfVec3f(
					static_cast<float>(world_normal[0]),
					static_cast<float>(world_normal[1]),
					static_cast<float>(world_normal[2]));
				usd_normals_.push_back(N);

				// UV
				if (modo_has_uvs_)
				{
					float uv[2];
					if (!PolyMapValue(uv, pnt))
					{
						uv[0] = uv[1] = 0.0f;
					}
					auto UV = pxr::GfVec2f(uv[0], uv[1]);
					usd_uvs_.push_back(UV);
				}
			}
		}

	private:
		void WriteMesh()
		{
			// Pathを作成
			pxr::SdfPath mesh_path("/root");
			std::string mesh_name = ItemName();
			mesh_name = GetValidPathName(mesh_name);
			mesh_path = mesh_path.AppendChild(pxr::TfToken(mesh_name.c_str()));
			auto mesh = pxr::UsdGeomMesh::Define(usd_stage_, mesh_path);
			usd_curr_mesh_path_ = mesh_path;

			// 行列と座標を取得
			WorldXform(current_mtx_, current_pos_);
			current_mtx_determinant_ = lx::MatrixDeterminant(current_mtx_);

			// 座標のイテレート
			{
				usd_points_.clear();
				mesh_point_id_to_index_.clear();
				mesh_point_count_ = 0;

				WritePoints();

				mesh.GetPointsAttr().Set(usd_points_);
			}

			// フェイスのイテレート
			{
				usd_index_count_by_face_.clear();
				usd_face_indices_.clear();
				usd_normals_.clear();
				modo_has_uvs_ = SetSelMap(LXi_VMAP_TEXTUREUV);
				if (!modo_has_uvs_)
				{
					modo_has_uvs_ = SetMap(LXi_VMAP_TEXTUREUV);
				}

				WritePolys();

				mesh.GetFaceVertexCountsAttr().Set(usd_index_count_by_face_);
				mesh.GetFaceVertexIndicesAttr().Set(usd_face_indices_);
				mesh.GetNormalsAttr().Set(usd_normals_);
				mesh.SetNormalsInterpolation(pxr::UsdGeomTokens->faceVarying);

				auto st_token = pxr::UsdUtilsGetPrimaryUVSetName();
				auto st_prim_var = mesh.CreatePrimvar(st_token, pxr::SdfValueTypeNames->Float2Array, pxr::UsdGeomTokens->faceVarying);
				st_prim_var.Set(usd_uvs_);
			}
		}

		void WriteMaterial()
		{
			// Rootパス直下にScopeを追加
			pxr::SdfPath scope_path("/root/looks");
			auto scope_prim = pxr::UsdGeomScope::Define(usd_stage_, scope_path).GetPrim();

			// TODO: 現在はマテリアル名とアサインされているポリゴン情報のみ
			for (auto&& mat_assign_info : modo_assign_material_and_face_index_)
			{
				// マテリアルパスを追加
				// NOTE: パス名として使えない文字が入ってくる場合があるので対応する
				auto mat_name = mat_assign_info.first;
				mat_name = GetValidPathName(mat_name);
				pxr::SdfPath mat_path = scope_path.AppendChild(pxr::TfToken(mat_name));
				auto mat = pxr::UsdShadeMaterial::Define(usd_stage_, mat_path);

				// マテリアルがアサインされているメッシュとポリゴンインデックスを登録
				for (auto&& path_face : mat_assign_info.second)
				{
					auto bound_prim = usd_stage_->OverridePrim(path_face.first);
					auto face_set = mat.CreateMaterialFaceSet(bound_prim);
					face_set.AppendFaceGroup(path_face.second, mat_path);
				}

				// MODO内のマテリアル情報を取得する
				if (ScanMask(mat_assign_info.first.c_str()))
				{
					CLxUser_Item matr, cmap, bmap, tmap, smap, lmap;
					const char* fx = nullptr;
					while (NextLayer()) {
						if (!ChanInt(LXsICHAN_TEXTURELAYER_ENABLE))
							continue;

						if (ItemIsA(LXsITYPE_ADVANCEDMATERIAL))
							GetItem(matr);

						else if (ItemIsA(LXsITYPE_IMAGEMAP)) {
							fx = LayerEffect();

							if (!strcmp(fx, LXs_FX_DIFFCOLOR))
								GetItem(cmap);

							else if (!strcmp(fx, LXs_FX_SPECCOLOR))
								GetItem(smap);

							else if (!strcmp(fx, LXs_FX_TRANCOLOR))
								GetItem(tmap);

							else if (!strcmp(fx, LXs_FX_LUMICOLOR))
								GetItem(lmap);

							else if (!strcmp(fx, LXs_FX_BUMP))
								GetItem(bmap);
						}
					}
					if (matr.test() && SetItem(matr))
					{
						// ディフューズカラーをDisplayColorとして出力
						auto a = ChanFloat(LXsICHAN_ADVANCEDMATERIAL_DIFFAMT);
						auto idx = ChanIndex(LXsICHAN_ADVANCEDMATERIAL_DIFFCOL".R");
						pxr::GfVec3f diff_color(
							static_cast<float>(ChanFloat(idx + 0) * a),
							static_cast<float>(ChanFloat(idx + 1) * a),
							static_cast<float>(ChanFloat(idx + 2) * a));
						diff_color = pxr::GfConvertDisplayToLinear(diff_color);

						auto dispColorIA = mat.CreateInput(pxr::_tokens->displayColor, pxr::SdfValueTypeNames->Color3f);
						dispColorIA.Set(pxr::VtValue(diff_color));

						auto mat_prim = mat.GetPrim();
						std::string shader_name = pxr::TfStringPrintf("%s_lambert", mat_prim.GetName().GetText());
						pxr::TfToken shader_prim_name(shader_name);
						auto bxdf_schema = pxr::UsdRiRisBxdf::Define(usd_stage_, mat_prim.GetPath().AppendChild(shader_prim_name));
						bxdf_schema.CreateFilePathAttr(pxr::VtValue(pxr::SdfAssetPath("PxrDiffuse")));
						auto diffuse = bxdf_schema.CreateInput(pxr::_tokens->diffuseColor, pxr::SdfValueTypeNames->Color3f);
						pxr::UsdRiMaterialAPI(mat).SetInterfaceInputConsumer(dispColorIA, diffuse);
					}
				}
			}
		}

	private:
		std::string		usd_file_name_ = "";
		bool			can_output_ = false;

		double			unit_scale_;
		LXtMatrix		current_mtx_;
		double			current_mtx_determinant_;
		LXtVector		current_pos_;

		typedef std::map<pxr::SdfPath, pxr::VtArray<int>>	PathFaceArray;

		pxr::SdfPath					usd_curr_mesh_path_;
		pxr::UsdStageRefPtr				usd_stage_;
		pxr::VtArray<pxr::GfVec3f>		usd_points_;
		pxr::VtArray<pxr::GfVec3f>		usd_normals_;
		pxr::VtArray<pxr::GfVec2f>		usd_uvs_;
		pxr::VtArray<int>				usd_index_count_by_face_;
		pxr::VtArray<int>				usd_face_indices_;
		std::map<LXtPointID, unsigned>	mesh_point_id_to_index_;
		unsigned int					mesh_point_count_;
		bool							modo_has_uvs_;
		std::map<std::string, PathFaceArray>		modo_assign_material_and_face_index_;
	};	// class CUsdSaver

	LXtTagInfoDesc	 CUsdSaver::descInfo[]
	{
		{ LXsSAV_OUTCLASS,	LXa_SCENE },
		{ LXsSAV_DOSTYPE,	"usda" },
		{ LXsSRV_USERNAME,	"Pixer USD ascii" },
		{ LXsSRV_LOGSUBSYSTEM,	"io-status" },
		{ 0 }
	};
}

void initialize()
{
	LXx_ADD_SERVER(Saver, ModoUsd::CUsdSaver, "pxr_USD");
}
