/*
 *
 * Copyright 2018 Asylo authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include "asylo/examples/grpc_server/translator_server_impl.h"

#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "include/grpcpp/grpcpp.h"

namespace examples {
namespace grpc_server {

TranslatorServerImpl::TranslatorServerImpl()
    : Service(),
      // Initialize the translation map with a few known translations.
      translation_map_({{"asylo", "sanctuary"},
                        {"istio", "sail"},
                        {"kubernetes", "helmsman"}}) {}

::grpc::Status TranslatorServerImpl::GetTranslation(
    ::grpc::ServerContext *context, const GetTranslationRequest *request,
    GetTranslationResponse *response) {
  // Confirm that |*request| has an |input_word| field.
  if (!request->has_input_word()) {
    return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                          "No input word given");
  }

  // Confirm that the translation map has a translation for the input word.
  auto response_iterator =
      translation_map_.find(absl::AsciiStrToLower(request->input_word()));
  if (response_iterator == translation_map_.end()) {
    return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                          absl::StrCat("No known translation for \"",
                                       request->input_word(), "\""));
  }

  // Return the translation.
  response->set_translated_word(response_iterator->second);
  return ::grpc::Status::OK;
}

double **matrix1, **matrix2, **matrix_result;
double x[2][5]={{1,0,3,0,5},{0,2,0,4,0}};
double y[1][5] = {1,2,3,4,5};
double **w;
double *b;
std::string output;

int row_mat1, row_mat2;
int col_mat1, col_mat2;


int w_row, w_col;
int x_row, x_col;
int y_row, y_col;
int b_size;

void TranslatorServerImpl::split(const std::string &str, std::vector<std::string> &vect, char ch)
{
	int pos = str.find(ch);
	int initialPos = 0;
	int count = 0;
	vect.clear();
	
	while (pos != std::string::npos)	{
		vect.push_back(str.substr(initialPos, pos - initialPos));
		count++;
		initialPos = pos + 1;

		pos = str.find(ch, initialPos);
	}
	int min = (pos < str.size()) ? pos : str.size();
	vect.push_back(str.substr(initialPos, min - initialPos + 1));
	count++;
}

void TranslatorServerImpl::getDim(std::string size, int &count_rows, int &count_columns)
{
	std::vector<std::string> vect_shape;
	split(size, vect_shape, ',');
	count_rows = atoi(vect_shape[0].erase(0, 1).c_str());
	count_columns = atoi(vect_shape[1].erase((vect_shape[1].length() - 1), 1).c_str());
}

void TranslatorServerImpl::getDim(std::string size, int &vec_size)
{
	std::vector<std::string> vec_shape;
	split(size, vec_shape, ',');
	vec_size = atoi(vec_shape[0].erase(0,1).c_str());
}

double **TranslatorServerImpl::getMat(std::string size, std::vector<double> input, int &count_rows, int &count_columns)
{
	double **ary;
	ary = new double *[count_rows];
	
  for (int i = 0; i < count_rows; ++i)
		ary[i] = new double[count_columns];

	std::vector<double> vect_values = input;

	for (int row = 0; row < count_rows; row++){
		for (int column = 0; column < count_columns; column++){
			ary[row][column] = input[count_columns * row + column];
		}
	}

	return ary;
}

double *TranslatorServerImpl::getVec(std::vector<double> input, int vec_size)
{
	double *ary = new double [vec_size];
	for (int i = 0; i<vec_size;i++)
		ary[i] = input[i];
	
	return ary;
}

double **TranslatorServerImpl::transpose(std::vector<double> input, int &count_rows, int &count_columns)
{
	double **ary;
	int temp;
	temp = count_rows;
	count_rows = count_columns;
	count_columns = temp;
  ary = new double *[count_rows];
	std::vector<double> vect_values = input;

  for (int i = 0; i < count_rows; ++i)
		ary[i] = new double[count_columns];

	for (int row = 0; row < count_rows; row++){
		for (int column = 0; column < count_columns; column++){
			ary[row][column] = input[row + count_columns * column];
		}
	}
	return ary;
}

double **TranslatorServerImpl::matmul(double** input_mat1, double input_mat2[2][5], int output_row, int output_col, int inner)
{
	double **result_mat;
  result_mat = new double *[output_row];

	for (int i = 0; i < row_mat1; i++){
		result_mat[i] = new double[output_col];
	}

	double _temp = 0;

	for (int i = 0; i < output_row; i++){
		for (int j = 0; j < output_col; j++){
			for (int k = 0; k < inner; k++){
				_temp += (input_mat1[i][k] * input_mat2[k][j]);
			}
			result_mat[i][j] = _temp;
			_temp = 0;
		}
	}
	return result_mat;
}

void TranslatorServerImpl::matadd(double **input_mat1, double *vec, int mat_row, int mat_col)
{
	for(int i = 0; i<mat_row;i++)
	{
		for(int j = 0; j < mat_col; j++)
		{
			input_mat1[i][j] += vec[0];
		}
	}
}

void TranslatorServerImpl::matsub(double **input_mat1, double vec[1][5], int mat_row, int mat_col)
{
	for(int i = 0; i<mat_row;i++)
	{
		for(int j = 0; j < mat_col; j++)
		{
			input_mat1[i][j] -= vec[i][j];
		}
	}
}

void TranslatorServerImpl::getOutput()
{
	output = "";

	for (int i = 0; i < row_mat1; i++){
		output += "[";
		for (int j = 0; j < col_mat2 - 1; j++){
			output += std::to_string(matrix_result[i][j]) + " ";
		}
		output += std::to_string(matrix_result[i][col_mat2 - 1]);
		output += "]";
	}
}

void TranslatorServerImpl::setOutput(GetMatmulResponse *response, int output_row, int output_col)
{
	for (int i = 0; i < output_row; i++){
		for (int j = 0; j < output_col; j++)
			response->add_result(matrix_result[i][j]); //add each result to response var
	}
}

void TranslatorServerImpl::deleteMemory()
{
	for (int i = 0; i < row_mat1; i++){
		delete[] matrix1[i];
    delete[] matrix_result[i];
	}

	for (int i = 0; i < row_mat2; i++){
		delete[] matrix2[i];
	}
}

::grpc::Status TranslatorServerImpl::MatMul(
	::grpc::ServerContext *context, const GetMatMulRequest *request,
	GetMatmulResponse *response)
{
	context->set_compression_algorithm(GRPC_COMPRESS_GZIP);

	row_mat1 = 0;
	col_mat1 = 0;
	row_mat2 = 0;
	col_mat2 = 0;

	std::string w_size_str = request->tensor1_shape();
	std::string b_size_str = request->tensor2_shape();
	std::string x_size_str = "[2,5]";
	std::string y_size_str = "[1,5]";
	std::vector<double> w_input;
	std::vector<double> x_input;
	std::vector<double> b_input;
	std::vector<double> y_input;

	getDim(w_size_str, w_row, w_col);
	getDim(x_size_str, x_row, x_col);
	getDim(y_size_str, y_row, y_col);

	getDim(b_size_str, b_size);

	for (int i = 0; i < w_row * w_col; i++)
		w_input.push_back(request->tensor1(i));
	for (int i = 0; i < b_size ; i++)
		b_input.push_back(request->tensor2(i));
	
	if (col_mat1 != row_mat2){
		/*
		if (col_mat1 == col_mat2){
			matrix1 = getMat(input_size1, input_str1, row_mat1, col_mat1);
			matrix2 = transpose(input_str2, row_mat2, col_mat2);
		}
		else if (row_mat2 == row_mat1){
			matrix1 = transpose(input_str1, row_mat1, col_mat1);
			matrix2 = getMat(input_size2, input_str2, row_mat2, col_mat2);
		}*/
	}
	else{
		w = getMat(w_size_str, w_input, w_row, w_col);
		b = getVec(b_input, b_size);
	}

	matrix_result = matmul(w, x,1,5,2);
	matadd(matrix_result, b, 1, 5);
	matsub(matrix_result, y, 1, 5);

	if (matrix_result != NULL){
		setOutput(response,1,5);
	}
	else{
		std::cout << "matrix_result is NULL" << std::endl;
		output = " ";
	}
	deleteMemory();
	return ::grpc::Status::OK;
}

}  // namespace grpc_server
}  // namespace examples
